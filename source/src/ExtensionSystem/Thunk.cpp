// Windows x64 运行时 thunk 生成器：按参数/返回值描述生成机器码，把统一参数数组翻译成目标 DLL 的真实调用约定

#include "ExtensionSystem/Thunk.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
	// emitMachineCode() 用它做越界护栏
	constexpr std::size_t kThunkPageSize = 4096;

	class X86Writer
	{
	public:
		explicit X86Writer(std::uint8_t* buffer)
			: m_buffer(buffer)
		{
		}

		void u8(std::uint8_t v)
		{
			if (m_buffer)
				m_buffer[m_size] = v;
			++m_size;
		}

		std::size_t size() const
		{
			return m_size;
		}

		void pushRbx() { u8(0x53); }
		void popRbx() { u8(0x5B); }
		void ret() { u8(0xC3); }

		void movR10Rcx()
		{
			u8(0x4C); u8(0x8B); u8(0xD1);
		}

		void movRbxRdx()
		{
			u8(0x48); u8(0x8B); u8(0xDA);
		}

		// 栈帧调整必须用 imm32（48 81 EC id），不能改回 imm8（48 83 EC ib）：imm8 是符号扩展的，
		// stackAlloc 在 15/16 参数时正好是 128，编码成 0x80 会被当成 -128，栈参数被写到 rsp 之上
		void subRspImm32(std::uint32_t v)
		{
			u8(0x48); u8(0x81); u8(0xEC);
			u8(static_cast<std::uint8_t>(v));
			u8(static_cast<std::uint8_t>(v >> 8));
			u8(static_cast<std::uint8_t>(v >> 16));
			u8(static_cast<std::uint8_t>(v >> 24));
		}

		void addRspImm32(std::uint32_t v)
		{
			u8(0x48); u8(0x81); u8(0xC4);
			u8(static_cast<std::uint8_t>(v));
			u8(static_cast<std::uint8_t>(v >> 8));
			u8(static_cast<std::uint8_t>(v >> 16));
			u8(static_cast<std::uint8_t>(v >> 24));
		}

		void movGprFromR10Disp8(int regCode, bool rexR, std::uint8_t disp)
		{
			const std::uint8_t rex = 0x48 | (rexR ? 0x04 : 0x00) | 0x01;
			u8(rex);
			u8(0x8B);
			u8(0x40 | (regCode << 3) | 0x02);
			u8(disp);
		}

		void movGpr32FromR10Disp8(int regCode, bool rexR, std::uint8_t disp)
		{
			const std::uint8_t rex = 0x40 | (rexR ? 0x04 : 0x00) | 0x01;
			u8(rex);
			u8(0x8B);
			u8(0x40 | (regCode << 3) | 0x02);
			u8(disp);
		}

		void movRaxFromR10Disp8(std::uint8_t disp)
		{
			movGprFromR10Disp8(0, false, disp);
		}

		void movRcxFromR10Disp8(std::uint8_t disp)
		{
			movGprFromR10Disp8(1, false, disp);
		}

		void movRdxFromR10Disp8(std::uint8_t disp)
		{
			movGprFromR10Disp8(2, false, disp);
		}

		void movR8FromR10Disp8(std::uint8_t disp)
		{
			movGprFromR10Disp8(0, true, disp);
		}

		void movR9FromR10Disp8(std::uint8_t disp)
		{
			movGprFromR10Disp8(1, true, disp);
		}

		void movRcx32FromR10Disp8(std::uint8_t disp)
		{
			movGpr32FromR10Disp8(1, false, disp);
		}

		void movRdx32FromR10Disp8(std::uint8_t disp)
		{
			movGpr32FromR10Disp8(2, false, disp);
		}

		void movR8_32FromR10Disp8(std::uint8_t disp)
		{
			movGpr32FromR10Disp8(0, true, disp);
		}

		void movR9_32FromR10Disp8(std::uint8_t disp)
		{
			movGpr32FromR10Disp8(1, true, disp);
		}

		void movRax32FromR10Disp8(std::uint8_t disp)
		{
			movGpr32FromR10Disp8(0, false, disp);
		}

		void movsdXmmFromR10Disp8(int xmmIndex, std::uint8_t disp)
		{
			// F2 是 legacy 前缀，必须在 REX 之前
			u8(0xF2);
			u8(0x41);
			u8(0x0F);
			u8(0x10);
			u8(0x40 | (xmmIndex << 3) | 0x02);
			u8(disp);
		}

		void movRaxToRspDisp8(std::uint8_t disp)
		{
			u8(0x48);
			u8(0x89);
			u8(0x44);
			u8(0x24); // 一定要有，是 x64 确认偏移的 SIB 字节
			u8(disp);
		}

		void storeBoolToRbx()
		{
			u8(0x88); u8(0x03);
		}

		void storeIntToRbx()
		{
			u8(0x89); u8(0x03);
		}

		void storeDoubleToRbx()
		{
			u8(0x66); u8(0x0F); u8(0xD6); u8(0x03);
		}

		void storePointerToRbx()
		{
			u8(0x48); u8(0x89); u8(0x03);
		}

		void movRaxImm64(std::uint64_t v)
		{
			u8(0x48);
			u8(0xB8);
			for (int i = 0; i < 8; ++i)
				u8(static_cast<std::uint8_t>(v >> (i * 8)));
		}

		void callRax()
		{
			u8(0xFF);
			u8(0xD0);
		}

	private:
		std::uint8_t* m_buffer = nullptr;
		std::size_t m_size = 0;
	};
}

namespace Thunk
{
	Thunk::~Thunk()
	{
		if (m_code)
			VirtualFree(m_code, 0, MEM_RELEASE);
	}

	bool Thunk::build(const Signature& signature, void* target)
	{
		if (!target) {
			m_errorString = QStringLiteral("thunk target is null");
			return false;
		}

		m_errorString.clear();

		m_code = VirtualAlloc(nullptr, kThunkPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!m_code) {
			m_errorString = QStringLiteral("VirtualAlloc failed");
			return false;
		}

		if (!emitMachineCode(signature, target)) {
			m_errorString = QStringLiteral("emitMachineCode failed");
			VirtualFree(m_code, 0, MEM_RELEASE);
			m_code = nullptr;
			return false;
		}

		// 不要在这里再赋值 m_codeSize：它由 emitMachineCode() 末尾设置，否则长度会被覆盖成 0
		FlushInstructionCache(GetCurrentProcess(), m_code, m_codeSize);
		return true;
	}

	void Thunk::call(const Arg* args, void* result) const
	{
		if (!m_code || !args || !result)
			return;

		auto fn = reinterpret_cast<ThunkFn>(m_code);
		fn(args, result);
	}

	QString Thunk::errorString() const
	{
		return m_errorString;
	}

	bool Thunk::emitMachineCode(const Signature& signature, void* target)
	{
		if (signature.args.size() > 16) {
			m_errorString = QStringLiteral("thunk currently supports at most 16 arguments");
			return false;
		}

		X86Writer writer(static_cast<std::uint8_t*>(m_code));

		const int stackArgCount = signature.args.size() > 4 ? signature.args.size() - 4 : 0;
		const int stackBytes = stackArgCount * 8;
		// push rbx 已让 RSP 16 字节对齐，只需分配 16 的倍数
		const int stackAlloc = (32 + stackBytes + 15) & ~15;

		writer.pushRbx();
		writer.movR10Rcx();
		writer.movRbxRdx();
		writer.subRspImm32(static_cast<std::uint32_t>(stackAlloc));

		for (int i = 0; i < signature.args.size(); ++i) {
			const ArgType type = signature.args.at(i);
			const std::uint8_t disp = static_cast<std::uint8_t>(i * 8);

			if (i < 4) {
				switch (type) {
				case ArgType::Bool:
				case ArgType::Int:
				case ArgType::String:
				case ArgType::Pointer64:
					switch (i) {
					case 0: writer.movRcxFromR10Disp8(disp); break;
					case 1: writer.movRdxFromR10Disp8(disp); break;
					case 2: writer.movR8FromR10Disp8(disp); break;
					case 3: writer.movR9FromR10Disp8(disp); break;
					}
					break;
				case ArgType::Pointer32:
					switch (i) {
					case 0: writer.movRcx32FromR10Disp8(disp); break;
					case 1: writer.movRdx32FromR10Disp8(disp); break;
					case 2: writer.movR8_32FromR10Disp8(disp); break;
					case 3: writer.movR9_32FromR10Disp8(disp); break;
					}
					break;
				case ArgType::Double:
					writer.movsdXmmFromR10Disp8(i, disp);
					break;
				}
			}
			else {
				if (type == ArgType::Pointer32)
					writer.movRax32FromR10Disp8(disp);
				else
					writer.movRaxFromR10Disp8(disp);
				writer.movRaxToRspDisp8(static_cast<std::uint8_t>(32 + (i - 4) * 8));
			}
		}

		writer.movRaxImm64(reinterpret_cast<std::uintptr_t>(target));
		writer.callRax();

		switch (signature.returnType) {
		case ReturnType::Void:
			break;
		case ReturnType::Bool:
			writer.storeBoolToRbx();
			break;
		case ReturnType::Int:
			writer.storeIntToRbx();
			break;
		case ReturnType::Double:
			writer.storeDoubleToRbx();
			break;
		case ReturnType::String:
			writer.storePointerToRbx();
			break;
		}

		writer.addRspImm32(static_cast<std::uint32_t>(stackAlloc));
		writer.popRbx();
		writer.ret();

		// X86Writer 不做边界检查，越界说明编码逻辑被改坏了，宁可报错也不要写穿这一页
		if (writer.size() > kThunkPageSize) {
			m_errorString = QStringLiteral("generated thunk exceeds the allocated page");
			return false;
		}

		m_codeSize = writer.size();
		return true;
	}
}
