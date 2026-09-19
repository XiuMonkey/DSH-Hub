import { pathToFileURL } from 'node:url'

const [dshEntryPath, ...dshArguments] = process.argv.slice(2)

function report(label, value) {
  process.stderr.write(`[harness-node] ${label}: ${value}\n`)
}

process.on('uncaughtException', (error) => report('uncaught exception', error?.stack ?? error))
process.on('unhandledRejection', (error) => report('unhandled rejection', error?.stack ?? error))

process.stdout.write(
  `[harness-node] runtime node=${process.version} platform=${process.platform} arch=${process.arch}\n`
)
process.stdout.write(`[harness-node] execPath=${process.execPath}\n`)
process.stdout.write(`[harness-node] cwd=${process.cwd()}\n`)
process.stdout.write(`[harness-node] DSH_HOME=${process.env.DSH_HOME ?? ''}\n`)

if (!dshEntryPath) {
  report('startup error', 'missing DSH entry path')
  process.exitCode = 1
} else {
  process.stdout.write(`[harness-node] loading=${dshEntryPath}\n`)
  process.argv = [process.execPath, dshEntryPath, ...dshArguments]
  try {
    const loaded = await import(pathToFileURL(dshEntryPath).href)
    process.stdout.write('[harness-node] DSH entry loaded\n')

    // dsh 0.1.5 的 lib/bin.js 用 `if (import.meta.main) await runCli()` 自我派发：
    // import.meta.main 只在「node 直接启动该模块」时为 true，而本包装层是用
    // import() 加载它的，于是 runCli() 不会被调用 —— 模块加载完进程就以 0 退出，
    // 既不起 webserver 也不打印任何东西。
    //
    // 因此这里必须显式调用 bin.js 导出的 runCli()。这是 0.1.5 的固定契约
    // （客户端只支持 0.1.5 起的服务端）；若导出的名字变了，宁可响亮地失败。
    process.stdout.write('[harness-node] invoking exported runCli()\n')
    await loaded.runCli()
  } catch (error) {
    report('DSH entry failed', error?.stack ?? error)
    process.exitCode = 1
  }
}
