#include "Common.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/Interfaces/IConsole.h"
#include "Core/Shared/Video/VideoDecoder.h"
#include "Core/NES/NesDefaultVideoFilter.h"
#include "Core/Debugger/ScriptingContext.h"
#include "Utilities/Serializer.h"

extern unique_ptr<Emulator> _emu;

namespace
{
	string JsonEscape(const string& value)
	{
		std::ostringstream out;
		for(unsigned char c : value) {
			switch(c) {
				case '"': out << "\\\""; break;
				case '\\': out << "\\\\"; break;
				case '\b': out << "\\b"; break;
				case '\f': out << "\\f"; break;
				case '\n': out << "\\n"; break;
				case '\r': out << "\\r"; break;
				case '\t': out << "\\t"; break;
				default:
					if(c < 0x20) {
						out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
					} else {
						out << c;
					}
					break;
			}
		}
		return out.str();
	}

	string BuildStateJson()
	{
		Serializer serializer(0, true, SerializeFormat::Map);
		serializer.Stream(*_emu->GetConsole().get(), "", -1);

		uint32_t frameCount = _emu->GetFrameCount();
		uint64_t masterClock = _emu->GetMasterClock();
		uint32_t masterClockRate = _emu->GetMasterClockRate();
		string consoleType = string(magic_enum::enum_name<ConsoleType>(_emu->GetConsoleType()));
		serializer.Stream(frameCount, "frameCount");
		serializer.Stream(masterClock, "masterClock");
		serializer.Stream(masterClockRate, "masterClockRate");
		serializer.Stream(consoleType, "consoleType");

		vector<std::pair<string, SerializeMapValue>> values;
		for(auto& entry : serializer.GetMapValues()) {
			values.push_back(entry);
		}
		std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

		std::ostringstream json;
		json << "{\n";
		for(size_t i = 0; i < values.size(); i++) {
			const auto& entry = values[i];
			json << "  \"" << JsonEscape(entry.first) << "\": ";
			switch(entry.second.Format) {
				case SerializeMapValueFormat::Integer: json << entry.second.Value.Integer; break;
				case SerializeMapValueFormat::Double: json << std::setprecision(17) << entry.second.Value.Double; break;
				case SerializeMapValueFormat::Bool: json << (entry.second.Value.Bool ? "true" : "false"); break;
				case SerializeMapValueFormat::String: json << "\"" << JsonEscape(entry.second.StringValue) << "\""; break;
			}
			json << (i + 1 == values.size() ? "\n" : ",\n");
		}
		json << "}\n";
		return json.str();
	}

	bool WriteFile(const char* filename, const string& data)
	{
		if(filename == nullptr || filename[0] == 0) {
			return false;
		}
		ofstream file(filename, ios::binary | ios::trunc);
		if(!file) {
			return false;
		}
		file.write(data.data(), data.size());
		return file.good();
	}
}

extern "C"
{
	// Small, stable primitives used by the terminal-only agent front-end.  They
	// intentionally reuse the same emulator, serializer and video pipeline as
	// the GUI and Lua debugger instead of creating a second headless core.
	DllExport uint32_t __stdcall AgentGetFrameCount()
	{
		return _emu ? _emu->GetFrameCount() : 0;
	}

	DllExport uint64_t __stdcall AgentGetMasterClock()
	{
		return _emu ? _emu->GetMasterClock() : 0;
	}

	DllExport CpuType __stdcall AgentGetPrimaryCpuType()
	{
		if(_emu) {
			vector<CpuType> cpuTypes = _emu->GetCpuTypes();
			if(!cpuTypes.empty()) {
				return cpuTypes[0];
			}
		}
		return CpuType::Nes;
	}

	DllExport ConsoleType __stdcall AgentGetConsoleType()
	{
		return _emu ? _emu->GetConsoleType() : ConsoleType::Nes;
	}

	DllExport bool __stdcall AgentSaveScreenshot(const char* filename)
	{
		if(!_emu || !_emu->GetVideoDecoder()) {
			return false;
		}
		stringstream png;
		_emu->GetVideoDecoder()->WaitForAsyncFrameDecode();
		_emu->GetVideoDecoder()->TakeScreenshot(png);
		return !png.str().empty() && WriteFile(filename, png.str());
	}

	DllExport bool __stdcall AgentSaveStateJson(const char* filename)
	{
		if(!_emu || !_emu->GetConsole()) {
			return false;
		}
		return WriteFile(filename, BuildStateJson());
	}

	DllExport void __stdcall AgentInitializeHeadlessDefaults()
	{
		if(_emu) {
			//A terminal disassembly request must decode the requested address even
			//before the runtime/CDL cache has classified it.  The desktop defaults
			//hide unidentified and data-marked bytes because its view can render
			//collapsed blocks; JSONL output needs deterministic instruction rows.
			DebugConfig& debugConfig = _emu->GetSettings()->GetDebugConfig();
			debugConfig.DisassembleUnidentifiedData = true;
			debugConfig.DisassembleVerifiedData = true;

			//The desktop UI normally supplies its default NES palette through
			//ConfigApi.  A native CLI has no managed configuration layer, so use
			//the core's canonical 2C02 palette instead of the zero-initialized
			//interop buffer.
			NesConfig& nesConfig = _emu->GetSettings()->GetNesConfig();
			NesDefaultVideoFilter::InitializeDefaultPalette(nesConfig);
			std::fill(std::begin(nesConfig.ChannelVolumes), std::end(nesConfig.ChannelVolumes), 100);

			//The managed configuration layer also connects each system's standard
			//controller.  Keep special-device auto-detection intact: ROM metadata can
			//replace these defaults while the ROM is loading.
			nesConfig.Port1.Type = ControllerType::NesController;
			_emu->GetSettings()->GetSnesConfig().Port1.Type = ControllerType::SnesController;
			_emu->GetSettings()->GetGameboyConfig().Controller.Type = ControllerType::GameboyController;
			_emu->GetSettings()->GetGbaConfig().Controller.Type = ControllerType::GbaController;

			PcEngineConfig& pceConfig = _emu->GetSettings()->GetPcEngineConfig();
			pceConfig.Port1.Type = ControllerType::PceController;
			pceConfig.PreventSelectRunReset = true;
			//The desktop supplies a calibrated palette.  Use a complete native RGB333
			//palette in headless mode so PCE captures are usable without managed config.
			auto expand3BitColor = [](uint32_t value) { return value * 255 / 7; };
			for(uint32_t color = 0; color < 512; color++) {
				uint32_t blue = expand3BitColor(color & 0x07);
				uint32_t red = expand3BitColor((color >> 3) & 0x07);
				uint32_t green = expand3BitColor((color >> 6) & 0x07);
				pceConfig.Palette[color] = 0xFF000000 | (red << 16) | (green << 8) | blue;
			}

			SmsConfig& smsConfig = _emu->GetSettings()->GetSmsConfig();
			smsConfig.Port1.Type = ControllerType::SmsController;
			smsConfig.UseSgPalette = true;
			std::fill(std::begin(smsConfig.ChannelVolumes), std::end(smsConfig.ChannelVolumes), 100);

			CvConfig& cvConfig = _emu->GetSettings()->GetCvConfig();
			cvConfig.Port1.Type = ControllerType::ColecoVisionController;
			std::fill(std::begin(cvConfig.ChannelVolumes), std::end(cvConfig.ChannelVolumes), 100);

			WsConfig& wsConfig = _emu->GetSettings()->GetWsConfig();
			wsConfig.ControllerHorizontal.Type = ControllerType::WsController;
			wsConfig.ControllerVertical.Type = ControllerType::WsControllerVertical;
		}
	}

	DllExport void __stdcall AgentSetLuaAccess(bool allowIoOs, bool allowNetwork, uint32_t timeoutSeconds)
	{
		if(_emu) {
			ScriptingContext::SetBreakOnError(true);
			DebugConfig& config = _emu->GetSettings()->GetDebugConfig();
			config.ScriptAllowIoOsAccess = allowIoOs;
			config.ScriptAllowNetworkAccess = allowNetwork;
			config.ScriptTimeout = std::max<uint32_t>(1, timeoutSeconds);
		}
	}
}
