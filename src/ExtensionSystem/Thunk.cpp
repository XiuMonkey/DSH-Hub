// ------------------------------------------------------------------
// Thunk.cpp
// ------------------------------------------------------------------
// Windows x64 运行时 thunk 生成器。
// 根据参数/返回值描述生成可执行机器码，把统一参数数组转换为
// 目标 DLL 函数的真实调用约定（由 DllCaller 的 native 风格使用）。
// ------------------------------------------------------------------

#include "ExtensionSystem/Thunk.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace
{
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

		void subRspImm8(std::uint8_t v)
		{
			u8(0x48); u8(0x83); u8(0xEC); u8(v);
		}

		void addRspImm8(std::uint8_t v)
		{
			u8(0x48); u8(0x83); u8(0xC4); u8(v);
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
			// 没有 REX.W，加载 32 位并自动零扩展为 64 位
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
			// 前缀顺序：F2 是 legacy 前缀，REX 在它后面
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
			u8(0x24); //这个一定要有，是x64确认偏移的SIB字节
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
} // namespace

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

		// 直接使用足够大的页，避免精确计算大小。
		const std::size_t pageSize = 4096;
		m_code = VirtualAlloc(nullptr, pageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
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

		// ⚠️ 不要在这里再赋值 m_codeSize：它由 emitMachineCode() 末尾设置（那边有它
		// 自己的 X86Writer）。原先这里写的是 m_codeSize = writer.size()，可是外层那个
		// writer 一个字节都没 emit 过 ⇒ 长度被覆盖成 0 ⇒ 下面刷新 0 字节。
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
		// push rbx 已经让 RSP 16 字节对齐，这里只需要分配 16 的倍数。
		int stackAlloc = (32 + stackBytes + 15) & ~15;

		writer.pushRbx();
		writer.movR10Rcx();
		writer.movRbxRdx();
		writer.subRspImm8(static_cast<std::uint8_t>(stackAlloc));

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

		writer.addRspImm8(static_cast<std::uint8_t>(stackAlloc));
		writer.popRbx();
		writer.ret();

		m_codeSize = writer.size();
		return true;
	}
} // namespace Thunk