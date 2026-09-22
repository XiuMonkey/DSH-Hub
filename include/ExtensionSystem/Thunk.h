#pragma once

// ------------------------------------------------------------------
// Thunk.h
// ------------------------------------------------------------------
// Windows x64 运行时 thunk 生成器。
//
// 用途：在运行时根据参数/返回值描述生成一段可执行机器码，
// 将统一参数数组转换为目标 DLL 函数的真实调用约定。
// 由 DllCaller 的 native 调用风格（invokeNativeFunction）使用。
// ------------------------------------------------------------------

#include <QVector>
#include <QString>

#include <cstddef>
#include <cstdint>

namespace Thunk
{
	enum class ArgType
	{
		Bool,
		Int,
		Double,
		String,
		Pointer32,
		Pointer64
	};

	enum class ReturnType
	{
		Void,
		Bool,
		Int,
		Double,
		String
	};

	struct Signature
	{
		ReturnType returnType = ReturnType::Void;
		QVector<ArgType> args;
	};

	// 统一 thunk 参数：每个参数固定占 8 字节。
	// Bool/Int/String 使用 int64/pointer，Double 使用 double 的位模式。
	struct Arg
	{
		union
		{
			std::int64_t i = 0;
			double d;
			const char* s;
			std::uint32_t u32;
		} as;
	};

	// 生成的 thunk 入口统一使用该签名。
	// args 指向参数数组，result 指向返回值缓冲区。
	using ThunkFn = void (*)(const Arg* args, void* result);

	class Thunk
	{
	public:
		Thunk() = default;
		~Thunk();

		Thunk(const Thunk&) = delete;
		Thunk& operator=(const Thunk&) = delete;

		// 根据签名和目标函数地址生成 thunk。
		// 成功返回 true，失败可通过 errorString() 获取原因。
		bool build(const Signature& signature, void* target);

		// 调用生成的 thunk。
		void call(const Arg* args, void* result) const;

		QString errorString() const;

	private:
		bool emitMachineCode(const Signature& signature, void* target);

		void* m_code = nullptr;
		std::size_t m_codeSize = 0;
		QString m_errorString;
	};
} // namespace Thunk
