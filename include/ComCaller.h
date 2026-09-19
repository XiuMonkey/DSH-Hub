// ------------------------------------------------------------------
// ComCaller.h
// ------------------------------------------------------------------
// InterfaceType = "com" 的独立执行器（MVP：面向"简单函数"）。
// 命名不含 "Json"：参数以 JSON 形式传输，但执行的是 COM 自动化
// （IDispatch），与 DllCaller 的 json/native 风格是并列关系。
//
// 组件（ProgId）在 regulation.json5 的 "Com" 段里白名单式声明，
// 运行时 args 只能控制 member/path/params/value，不能任意指定对象：
//
//   regulation.json5:
//   {
//     "Tool": "com_calc_add",
//     "InterfaceType": "com",
//     "Com": { "ProgId": "My.Calc" }        // 组件在此固定
//   }
//
//   调用 args（经管道/HTTP 等传入）:
//   {
//     "member": "Add",            // 方法或属性名（必填）
//     "kind":   "method",         // method(默认) | get | put
//     "path":   ["PropA"],        // 可选：先沿属性链下行到目标对象
//     "params": [1, 2, "x"],      // kind=method 的参数（顺序，JSON 标量）
//     "value":  42                // kind=put 要写入的值（JSON 标量）
//   }
//
// 返回（写入 DllCaller 的 result）：
//   成功 -> { "value": <JSON 标量> }；结果为对象时附带 "object": true（仅摘要）
//   失败 -> invoke 返回 false，error 给出可读信息
//
// 说明：当前仅支持无状态的"一次一调"（每次新建组件实例、无对象句柄保活），
// 返回标量或常见整型集合计数；对象继续下钻的会话能力留待后续版本。
// ------------------------------------------------------------------
#pragma once

#include <QJsonObject>
#include <QString>

namespace comcall
{
	// 执行一次 COM 调用。成功返回 true 并把结果写入 result；
	// 失败返回 false 且 error 携带错误信息。
	bool invoke(const QJsonObject& comConfig,
		const QJsonObject& args,
		QJsonObject& result,
		QString& error);
} // namespace comcall
