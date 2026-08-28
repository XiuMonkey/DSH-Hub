// ------------------------------------------------------------------
// dsh-pipe-bridge
// ------------------------------------------------------------------
// DSH Node server -> DSH Hub Qt client 命名管道桥接插件。
//
// 协议：JSON + '\n'
//   请求: {"id":1,"tool":"ping","args":{}}
//   响应: {"id":1,"ok":true,"result":{"pong":true}}
//
// 用法：在 agent 工具列表里会看到：
//   pipe_ping  - 测试命名管道是否连通
//   pipe_call  - 调用 DSH Hub 客户端 DLL 桥接暴露的工具
// ------------------------------------------------------------------

import net from "node:net";
import { defineTool } from "@deepseek-ai/dsh-tools";

const PIPE_PATH = "\\\\.\\pipe\\dshhub-bridge";
const REQUEST_TIMEOUT_MS = 10000;

class PipeBridgeClient {
    constructor() {
        this.socket = null;
        this.buffer = "";
        this.pending = new Map();
        this.nextId = 1;
        this.connectPromise = null;
    }

    connect() {
        if (this.socket && !this.socket.destroyed) {
            return Promise.resolve();
        }

        if (this.connectPromise) {
            return this.connectPromise;
        }

        this.connectPromise = new Promise((resolve, reject) => {
            const socket = net.connect(PIPE_PATH, () => {
                this.socket = socket;
                this.connectPromise = null;
                resolve();
            });

            socket.on("data", (chunk) => this.handleData(chunk));

            socket.on("error", (err) => {
                this.socket = null;
                this.connectPromise = null;
                for (const entry of this.pending.values()) {
                    entry.reject(err);
                }
                this.pending.clear();
                reject(err);
            });

            socket.on("close", () => {
                this.socket = null;
                this.connectPromise = null;
                for (const entry of this.pending.values()) {
                    entry.reject(new Error("named pipe closed"));
                }
                this.pending.clear();
            });

            socket.setTimeout(REQUEST_TIMEOUT_MS, () => {
                socket.destroy(new Error("named pipe request timeout"));
            });
        });

        return this.connectPromise;
    }

    handleData(chunk) {
        this.buffer += chunk.toString();

        let newlineIndex;
        while ((newlineIndex = this.buffer.indexOf("\n")) !== -1) {
            const line = this.buffer.slice(0, newlineIndex).trim();
            this.buffer = this.buffer.slice(newlineIndex + 1);

            if (!line) {
                continue;
            }

            let response;
            try {
                response = JSON.parse(line);
            } catch {
                continue;
            }

            const entry = this.pending.get(response.id);
            if (!entry) {
                continue;
            }

            this.pending.delete(response.id);
            if (entry.timer) {
                clearTimeout(entry.timer);
            }

            if (response.ok) {
                entry.resolve(response.result || {});
            } else {
                entry.reject(new Error(response.error || "pipe call failed"));
            }
        }
    }

    call(tool, args = {}) {
        return this.connect().then(() => new Promise((resolve, reject) => {
            const id = this.nextId++;

            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error(`named pipe request timeout: ${tool}`));
            }, REQUEST_TIMEOUT_MS);

            this.pending.set(id, { resolve, reject, timer });

            this.socket.write(JSON.stringify({ id, tool, args }) + "\n");
        }));
    }

    close() {
        if (this.connectPromise) {
            this.connectPromise.catch(() => {});
            this.connectPromise = null;
        }

        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
        }
    }
}

export const name = "AttachedPlugin";
export const inject = ["tools"];

export function apply(ctx) {
    const bridge = new PipeBridgeClient();

    ctx.on("dispose", () => bridge.close());

    // 1. 测试命名管道连通性
    ctx.tools.register(defineTool({
        name: "pipe_ping",
        description: "Test communication with the DSH Hub client named pipe bridge.",
        parameters: {},
        output: {
            schema: {
                type: "object",
                additionalProperties: false,
                properties: {
                    pong: {
                        type: "boolean",
                        required: true
                    }
                }
            },
            render: (_args, value) => [
                { type: "text", text: `pong=${value.pong}` }
            ]
        },
        isConcurrencySafe: () => true,
        async execute() {
            return bridge.call("ping");
        }
    }));

    // 2. 通用调用：把任意工具名转发给 DSH Hub 客户端的 DLL 桥
    ctx.tools.register(defineTool({
        name: "pipe_call",
        description: "Call a tool exposed by the DSH Hub client's native DLL bridge over named pipe.",
        parameters: {
            tool: {
                type: "string",
                required: true,
                description: "Native tool name to invoke in the DSH Hub client."
            },
            args: {
                type: "object",
                additionalProperties: true,
                description: "Arguments as a JSON object."
            }
        },
        output: {
            schema: {
                type: "object",
                additionalProperties: true
            },
            render: (_args, value) => [
                { type: "text", text: JSON.stringify(value) }
            ]
        },
        isConcurrencySafe: () => false,
        async execute(args) {
            const result = await bridge.call(args.tool, args.args || {});
            return result;
        }
    }));

    // 3. Demo DLL 工具：dll_add
    ctx.tools.register(defineTool({
        name: "dll_add",
        description: "Call the ExampleDll add(double a, double b) function through the DSH Hub client named pipe.",
        parameters: {
            a: {
                type: "number",
                required: true,
                description: "First addend."
            },
            b: {
                type: "number",
                required: true,
                description: "Second addend."
            }
        },
        output: {
            schema: {
                type: "object",
                additionalProperties: false,
                properties: {
                    value: {
                        type: "number",
                        required: true
                    }
                }
            },
            render: (_args, value) => [
                { type: "text", text: `add result = ${value.value}` }
            ]
        },
        isConcurrencySafe: () => true,
        async execute(args) {
            return bridge.call("dll_add", { a: args.a, b: args.b });
        }
    }));

    // 4. Demo DLL 工具：dll_multiply
    ctx.tools.register(defineTool({
        name: "dll_multiply",
        description: "Call the ExampleDll multiply(int a, int b) function through the DSH Hub client named pipe.",
        parameters: {
            a: {
                type: "integer",
                required: true,
                description: "First factor."
            },
            b: {
                type: "integer",
                required: true,
                description: "Second factor."
            }
        },
        output: {
            schema: {
                type: "object",
                additionalProperties: false,
                properties: {
                    value: {
                        type: "integer",
                        required: true
                    }
                }
            },
            render: (_args, value) => [
                { type: "text", text: `multiply result = ${value.value}` }
            ]
        },
        isConcurrencySafe: () => true,
        async execute(args) {
            return bridge.call("dll_multiply", { a: args.a, b: args.b });
        }
    }));

    // 5. Demo DLL 工具：dll_echo
    ctx.tools.register(defineTool({
        name: "dll_echo",
        description: "Call the ExampleDll echo(const char* text) function through the DSH Hub client named pipe.",
        parameters: {
            text: {
                type: "string",
                required: true,
                description: "Text to echo."
            }
        },
        output: {
            schema: {
                type: "object",
                additionalProperties: false,
                properties: {
                    value: {
                        type: "string",
                        required: true
                    }
                }
            },
            render: (_args, value) => [
                { type: "text", text: String(value.value) }
            ]
        },
        isConcurrencySafe: () => true,
        async execute(args) {
            return bridge.call("dll_echo", { text: args.text });
        }
    }));
}
