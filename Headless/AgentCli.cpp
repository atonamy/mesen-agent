#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Core/Debugger/Breakpoint.h"
#include "Core/Debugger/DebugTypes.h"
#include "Core/Debugger/ExpressionEvaluator.h"
#include "Core/Debugger/ITraceLogger.h"
#include "Core/Shared/CpuType.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/MemoryType.h"
#include "Core/Shared/SettingTypes.h"
#include "Core/Shared/TimingInfo.h"
#include "Core/Shared/Video/VideoRenderer.h"
#include "Utilities/Video/AviWriter.h"
#include "Utilities/magic_enum.hpp"

namespace fs = std::filesystem;
using namespace std;

#ifdef _WIN32
#define AGENT_CALL __stdcall
#else
#define AGENT_CALL
#endif

extern "C" {
	void AGENT_CALL InitDll();
	void AGENT_CALL InitializeEmu(const char*, void*, void*, bool, bool, bool, bool);
	void AGENT_CALL Release();
	bool AGENT_CALL LoadRom(char*, char*);
	bool AGENT_CALL IsRunning();
	void AGENT_CALL Stop();
	int32_t AGENT_CALL GetStopCode();
	void AGENT_CALL Pause();
	void AGENT_CALL Resume();
	void AGENT_CALL SaveStateFile(char*);
	void AGENT_CALL LoadStateFile(char*);
	void AGENT_CALL GetLog(char*, uint32_t);
	uint32_t AGENT_CALL GetMesenVersion();
	void AGENT_CALL GetRomHash(HashType, char*, uint32_t);

	void AGENT_CALL SetEmulationFlag(EmulationFlags, bool);
	void AGENT_CALL SetDebuggerFlag(DebuggerFlags, bool);

	void AGENT_CALL InitializeDebugger();
	bool AGENT_CALL IsExecutionStopped();
	void AGENT_CALL ResumeExecution();
	void AGENT_CALL Step(CpuType, uint32_t, StepType);
	uint32_t AGENT_CALL GetProgramCounter(CpuType, bool);
	void AGENT_CALL SetProgramCounter(CpuType, uint32_t);
	int64_t AGENT_CALL EvaluateExpression(const char*, CpuType, EvalResultType*, bool);
	uint32_t AGENT_CALL GetMemorySize(MemoryType);
	void AGENT_CALL GetMemoryValues(MemoryType, uint32_t, uint32_t, uint8_t*);
	void AGENT_CALL SetMemoryValues(MemoryType, uint32_t, uint8_t*, int32_t);
	void AGENT_CALL SetBreakpoints(Breakpoint*, uint32_t);
	void AGENT_CALL SetInputSchedules(DebugInputSchedule*, uint32_t, uint32_t);
	uint32_t AGENT_CALL GetDisassemblyOutput(CpuType, uint32_t, CodeLineData*, uint32_t);
	void AGENT_CALL GetCallstack(CpuType, StackFrameInfo*, uint32_t&);
	void AGENT_CALL SetTraceOptions(CpuType, TraceLoggerOptions);
	void AGENT_CALL StartLogTraceToFile(const char*);
	void AGENT_CALL StopLogTraceToFile();
	void AGENT_CALL SaveCdlFile(MemoryType, char*);
	int32_t AGENT_CALL LoadScript(char*, char*, char*, int32_t);
	void AGENT_CALL RemoveScript(int32_t);
	void AGENT_CALL GetScriptLog(int32_t, char*, uint32_t);
	uint32_t AGENT_CALL GetScriptErrorCount(int32_t);

	void AGENT_CALL AviRecord(char*, RecordAviOptions);
	void AGENT_CALL AviStop();
	bool AGENT_CALL AviIsRecording();
	void AGENT_CALL WaveRecord(char*);
	void AGENT_CALL WaveStop();
	bool AGENT_CALL WaveIsRecording();

	uint32_t AGENT_CALL AgentGetFrameCount();
	uint64_t AGENT_CALL AgentGetMasterClock();
	CpuType AGENT_CALL AgentGetPrimaryCpuType();
	ConsoleType AGENT_CALL AgentGetConsoleType();
	bool AGENT_CALL AgentSaveScreenshot(const char*);
	bool AGENT_CALL AgentSaveStateJson(const char*);
	void AGENT_CALL AgentInitializeHeadlessDefaults();
	void AGENT_CALL AgentSetLuaAccess(bool, bool, uint32_t);
}

namespace
{
	struct MemoryRequest
	{
		string Type;
		uint32_t Start = 0;
		optional<uint32_t> Length;
		string File;
	};

	struct WriteRequest
	{
		string Type;
		uint32_t Start = 0;
		vector<uint8_t> Data;
	};

	struct DisassembleRequest
	{
		uint32_t Address = 0;
		uint32_t Lines = 16;
	};

	struct Options
	{
		string Rom;
		string Patch;
		string Home;
		string LoadState;
		string SaveState;
		string StateJson;
		string Screenshot;
		string Video;
		string Audio;
		string Trace;
		string Log;
		string Cpu;
		string StepTypeName = "step";
		string VideoCodecName;
		uint32_t Frames = 60;
		uint32_t StepCount = 0;
		uint32_t TimeoutSeconds = 30;
		uint32_t LuaTimeoutSeconds = 1;
		uint32_t CompressionLevel = 6;
		optional<uint32_t> SetPc;
		bool MaximumSpeed = true;
		bool LuaAllowIo = false;
		bool LuaAllowNetwork = false;
		bool ListMemory = false;
		bool ShowPc = false;
		bool ShowCallstack = false;
		bool VideoHud = false;
		bool InputHud = false;
		vector<string> LuaScripts;
		vector<string> Expressions;
		vector<string> Breakpoints;
		vector<MemoryRequest> Dumps;
		vector<MemoryRequest> Reads;
		vector<WriteRequest> Writes;
		vector<pair<string, string>> CdlFiles;
		vector<DisassembleRequest> Disassembly;
		vector<DebugInputSchedule> Inputs;
	};

	struct CliBreakpoint
	{
		uint32_t Id;
		CpuType Cpu;
		MemoryType Memory;
		BreakpointTypeFlags Type;
		int32_t Start;
		int32_t End;
		bool Enabled;
		bool MarkEvent;
		bool IgnoreDummyOperations;
		char Condition[1000];
	};

	static_assert(sizeof(CliBreakpoint) == sizeof(Breakpoint), "CLI breakpoint ABI must match core breakpoint ABI");

	string Lower(string value)
	{
		transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
		return value;
	}

	string JsonEscape(const string& value)
	{
		ostringstream out;
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
						out << "\\u" << hex << setw(4) << setfill('0') << (int)c << dec;
					} else {
						out << c;
					}
					break;
			}
		}
		return out.str();
	}

	void Event(const string& body)
	{
		cout << "{" << body << "}" << endl;
	}

	string ReadScriptLog(int32_t scriptId)
	{
		vector<char> log(1000000, 0);
		GetScriptLog(scriptId, log.data(), (uint32_t)log.size());
		return log.data();
	}

	void EmitScriptLog(int32_t scriptId, const string& scriptFile, uint32_t errorCount)
	{
		Event("\"event\":\"lua_log\",\"id\":" + to_string(scriptId) + ",\"file\":\"" + JsonEscape(fs::absolute(fs::u8path(scriptFile)).u8string()) + "\",\"error_count\":" + to_string(errorCount) + ",\"text\":\"" + JsonEscape(ReadScriptLog(scriptId)) + "\"");
	}

	[[noreturn]] void UsageError(const string& message)
	{
		throw invalid_argument(message);
	}

	uint32_t ParseNumber(string value)
	{
		if(!value.empty() && value[0] == '$') {
			value = "0x" + value.substr(1);
		}
		size_t consumed = 0;
		unsigned long long parsed = stoull(value, &consumed, 0);
		if(consumed != value.size() || parsed > UINT32_MAX) {
			UsageError("invalid 32-bit number: " + value);
		}
		return (uint32_t)parsed;
	}

	vector<string> Split(const string& value, char separator)
	{
		vector<string> result;
		stringstream input(value);
		string item;
		while(getline(input, item, separator)) {
			result.push_back(item);
		}
		return result;
	}

	template<typename T>
	optional<T> ParseEnum(const string& name)
	{
		string requested = Lower(name);
		for(auto entry : magic_enum::enum_entries<T>()) {
			if(Lower(string(entry.second)) == requested) {
				return entry.first;
			}
		}
		return nullopt;
	}

	MemoryType ParseMemoryType(const string& name, CpuType primaryCpu)
	{
		if(Lower(name) == "cpu") {
			switch(primaryCpu) {
				case CpuType::Snes: return MemoryType::SnesMemory;
				case CpuType::Spc: return MemoryType::SpcMemory;
				case CpuType::Sa1: return MemoryType::Sa1Memory;
				case CpuType::NecDsp: return MemoryType::NecDspMemory;
				case CpuType::Gsu: return MemoryType::GsuMemory;
				case CpuType::Cx4: return MemoryType::Cx4Memory;
				case CpuType::St018: return MemoryType::St018Memory;
				case CpuType::Gameboy: return MemoryType::GameboyMemory;
				case CpuType::Nes: return MemoryType::NesMemory;
				case CpuType::Pce: return MemoryType::PceMemory;
				case CpuType::Sms: return MemoryType::SmsMemory;
				case CpuType::Gba: return MemoryType::GbaMemory;
				case CpuType::Ws: return MemoryType::WsMemory;
			}
		}
		auto type = ParseEnum<MemoryType>(name);
		if(!type || *type == MemoryType::None) {
			UsageError("unknown memory type: " + name + " (use --list-memory after loading a ROM)");
		}
		return *type;
	}

	vector<uint8_t> ParseHexBytes(string value)
	{
		if(value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
			value = value.substr(2);
		}
		string hexValue;
		for(char c : value) {
			if(isxdigit((unsigned char)c)) {
				hexValue += c;
			} else if(c != ' ' && c != ',' && c != '_' && c != '-') {
				UsageError("invalid hex byte string: " + value);
			}
		}
		if(hexValue.empty() || hexValue.size() % 2 != 0) {
			UsageError("hex byte strings must contain whole bytes");
		}
		vector<uint8_t> result;
		for(size_t i = 0; i < hexValue.size(); i += 2) {
			result.push_back((uint8_t)stoul(hexValue.substr(i, 2), nullptr, 16));
		}
		return result;
	}

	string Hex(const uint8_t* data, size_t size)
	{
		ostringstream out;
		out << hex << setfill('0');
		for(size_t i = 0; i < size; i++) {
			out << setw(2) << (uint32_t)data[i];
		}
		return out.str();
	}

	void EnsureParent(const string& filename)
	{
		if(filename.empty()) {
			return;
		}
		fs::path parent = fs::absolute(fs::u8path(filename)).parent_path();
		if(!parent.empty()) {
			fs::create_directories(parent);
		}
	}

	void RequireArtifact(const string& filename, const string& kind, bool allowEmpty = false)
	{
		std::error_code error;
		uintmax_t size = fs::file_size(fs::u8path(filename), error);
		if(error || (!allowEmpty && size == 0)) {
			throw runtime_error(kind + " was not created: " + filename);
		}
	}

	string ReadTextFile(const string& filename)
	{
		std::ifstream input(fs::u8path(filename), ios::binary);
		if(!input) {
			throw runtime_error("could not read file: " + filename);
		}
		return string((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
	}

	void WriteBinaryFile(const string& filename, const vector<uint8_t>& data)
	{
		EnsureParent(filename);
		std::ofstream output(fs::u8path(filename), ios::binary | ios::trunc);
		if(!output) {
			throw runtime_error("could not create file: " + filename);
		}
		output.write((const char*)data.data(), data.size());
		if(!output.good()) {
			throw runtime_error("could not write file: " + filename);
		}
	}

	MemoryRequest ParseMemoryRequest(const string& value, bool fileRequired)
	{
		MemoryRequest result;
		string spec = value;
		if(fileRequired) {
			size_t equals = value.find('=');
			if(equals == string::npos || equals == 0 || equals + 1 == value.size()) {
				UsageError("memory dump syntax is TYPE[:START[:LENGTH]]=FILE");
			}
			spec = value.substr(0, equals);
			result.File = value.substr(equals + 1);
		}
		vector<string> fields = Split(spec, ':');
		if(fields.empty() || fields.size() > 3) {
			UsageError("memory syntax is TYPE[:START[:LENGTH]]");
		}
		result.Type = fields[0];
		if(fields.size() >= 2) result.Start = ParseNumber(fields[1]);
		if(fields.size() == 3) result.Length = ParseNumber(fields[2]);
		return result;
	}

	WriteRequest ParseWriteRequest(const string& value)
	{
		vector<string> fields = Split(value, ':');
		if(fields.size() != 3) {
			UsageError("memory write syntax is TYPE:START:HEXBYTES");
		}
		return { fields[0], ParseNumber(fields[1]), ParseHexBytes(fields[2]) };
	}

	DebugInputSchedule ParseInputSchedule(const string& value)
	{
		size_t colon = value.find(':');
		if(colon == string::npos) {
			UsageError("input syntax is PORT:BUTTON[,BUTTON]@START-END");
		}
		DebugInputSchedule schedule = { 0, {}, 0, UINT32_MAX };
		schedule.Port = ParseNumber(value.substr(0, colon));
		if(schedule.Port > 7) UsageError("input port must be 0 through 7");
		string buttonsAndRange = value.substr(colon + 1);
		size_t at = buttonsAndRange.find('@');
		string buttons = at == string::npos ? buttonsAndRange : buttonsAndRange.substr(0, at);
		if(at != string::npos) {
			string range = buttonsAndRange.substr(at + 1);
			size_t dash = range.find('-');
			if(dash == string::npos) {
				schedule.StartFrame = ParseNumber(range);
				schedule.EndFrame = schedule.StartFrame;
			} else {
				schedule.StartFrame = ParseNumber(range.substr(0, dash));
				schedule.EndFrame = ParseNumber(range.substr(dash + 1));
				if(schedule.EndFrame < schedule.StartFrame) UsageError("input end frame precedes start frame");
			}
		}
		for(string button : Split(buttons, ',')) {
			button = Lower(button);
			if(button == "a") schedule.State.A = true;
			else if(button == "b") schedule.State.B = true;
			else if(button == "x") schedule.State.X = true;
			else if(button == "y") schedule.State.Y = true;
			else if(button == "l") schedule.State.L = true;
			else if(button == "r") schedule.State.R = true;
			else if(button == "u") schedule.State.U = true;
			else if(button == "d") schedule.State.D = true;
			else if(button == "up") schedule.State.Up = true;
			else if(button == "down") schedule.State.Down = true;
			else if(button == "left") schedule.State.Left = true;
			else if(button == "right") schedule.State.Right = true;
			else if(button == "select") schedule.State.Select = true;
			else if(button == "start") schedule.State.Start = true;
			else UsageError("unknown input button: " + button);
		}
		if(!schedule.State.HasPressedButton()) UsageError("input schedule requires at least one button");
		return schedule;
	}

	void PrintHelp()
	{
		cout << R"HELP(Mesen Agent CLI - windowless emulator automation and debugging

Usage:
  mesen-agent ROM [options]

Execution:
  --frames N                    Run N frames (default: 60; 0 inspects initial state)
  --timeout SECONDS             Wall-clock safety timeout (default: 30)
  --no-max-speed                Use normal frame pacing
  --home DIR                    Isolated Mesen data/firmware/save directory
  --patch FILE                  Apply a ROM patch
  --load-state FILE             Load a .mss state before execution
  --save-state FILE             Save a .mss state after execution

Lua and input:
  --lua FILE                    Load a Mesen debugger Lua script (repeatable)
  --lua-allow-io                Allow Lua io/os libraries and script data folder
  --lua-allow-network           Allow Lua socket access
  --lua-timeout SECONDS         Per-callback Lua timeout (default: 1)
  --input PORT:BUTTONS@START-END  Override controller input over relative frames

Artifacts:
  --screenshot FILE.png         Save the last rendered frame
  --video FILE.avi|FILE.gif     Record gameplay through Mesen's recorder
  --video-codec cscd|zmbv|gif   Override codec inferred from extension
  --compression 0-9             AVI codec compression level (default: 6)
  --video-hud                   Include the system HUD in video
  --input-hud                   Include controller HUD in video
  --audio FILE.wav              Record emulated audio
  --state-json FILE.json        Dump the complete serialized console state
  --dump-memory TYPE[:START[:LENGTH]]=FILE
  --save-cdl TYPE=FILE          Save code/data logger data
  --trace FILE                  Save instruction trace
  --log FILE                    Save the emulator/debugger log

Debugging and inspection:
  --cpu TYPE                    CPU for expressions, stepping and disassembly
  --breakpoint TYPE:START[-END]:rwx[:CONDITION]
  --step N                      Step N operations after the frame run
  --step-type TYPE              step|over|out|cycle|ppu|scanline|frame|nmi|irq|back
  --eval EXPR                   Evaluate a debugger expression (repeatable)
  --read-memory TYPE:START:LENGTH
  --write-memory TYPE:START:HEXBYTES
  --disassemble ADDRESS[:LINES]
  --pc                          Print the program counter
  --set-pc ADDRESS              Set the program counter before execution
  --callstack                   Print the final call stack
  --list-memory                 List available memory spaces and sizes

TYPE names are case-insensitive Mesen enum names (for example NesMemory,
NesInternalRam, SnesWorkRam, GbaVideoRam).  The alias "cpu" selects the
relative address space for the active CPU.  All stdout records are JSON Lines.
)HELP";
	}

	Options ParseArguments(int argc, char** argv)
	{
		Options options;
		options.Home = (fs::temp_directory_path() / "mesen-agent").u8string();
		auto take = [&](int& index, const string& option) -> string {
			if(index + 1 >= argc) UsageError(option + " requires a value");
			return argv[++index];
		};

		for(int i = 1; i < argc; i++) {
			string arg = argv[i];
			if(arg == "--help" || arg == "-h") {
				PrintHelp();
				exit(0);
			} else if(arg == "--frames") options.Frames = ParseNumber(take(i, arg));
			else if(arg == "--timeout") options.TimeoutSeconds = ParseNumber(take(i, arg));
			else if(arg == "--home") options.Home = take(i, arg);
			else if(arg == "--patch") options.Patch = take(i, arg);
			else if(arg == "--load-state") options.LoadState = take(i, arg);
			else if(arg == "--save-state") options.SaveState = take(i, arg);
			else if(arg == "--state-json") options.StateJson = take(i, arg);
			else if(arg == "--screenshot") options.Screenshot = take(i, arg);
			else if(arg == "--video") options.Video = take(i, arg);
			else if(arg == "--video-codec") options.VideoCodecName = Lower(take(i, arg));
			else if(arg == "--audio") options.Audio = take(i, arg);
			else if(arg == "--trace") options.Trace = take(i, arg);
			else if(arg == "--log") options.Log = take(i, arg);
			else if(arg == "--cpu") options.Cpu = take(i, arg);
			else if(arg == "--step") options.StepCount = ParseNumber(take(i, arg));
			else if(arg == "--step-type") options.StepTypeName = Lower(take(i, arg));
			else if(arg == "--set-pc") options.SetPc = ParseNumber(take(i, arg));
			else if(arg == "--lua") options.LuaScripts.push_back(take(i, arg));
			else if(arg == "--lua-timeout") options.LuaTimeoutSeconds = ParseNumber(take(i, arg));
			else if(arg == "--eval") options.Expressions.push_back(take(i, arg));
			else if(arg == "--breakpoint") options.Breakpoints.push_back(take(i, arg));
			else if(arg == "--dump-memory") options.Dumps.push_back(ParseMemoryRequest(take(i, arg), true));
			else if(arg == "--read-memory") {
				MemoryRequest request = ParseMemoryRequest(take(i, arg), false);
				if(!request.Length) UsageError("memory read syntax is TYPE:START:LENGTH");
				options.Reads.push_back(request);
			}
			else if(arg == "--write-memory") options.Writes.push_back(ParseWriteRequest(take(i, arg)));
			else if(arg == "--save-cdl") {
				string value = take(i, arg);
				size_t equals = value.find('=');
				if(equals == string::npos) UsageError("CDL syntax is TYPE=FILE");
				options.CdlFiles.push_back({ value.substr(0, equals), value.substr(equals + 1) });
			} else if(arg == "--disassemble") {
				vector<string> fields = Split(take(i, arg), ':');
				if(fields.empty() || fields.size() > 2) UsageError("disassembly syntax is ADDRESS[:LINES]");
				options.Disassembly.push_back({ ParseNumber(fields[0]), fields.size() == 2 ? ParseNumber(fields[1]) : 16 });
			} else if(arg == "--input") options.Inputs.push_back(ParseInputSchedule(take(i, arg)));
			else if(arg == "--no-max-speed") options.MaximumSpeed = false;
			else if(arg == "--lua-allow-io") options.LuaAllowIo = true;
			else if(arg == "--lua-allow-network") options.LuaAllowNetwork = true;
			else if(arg == "--list-memory") options.ListMemory = true;
			else if(arg == "--pc") options.ShowPc = true;
			else if(arg == "--callstack") options.ShowCallstack = true;
			else if(arg == "--video-hud") options.VideoHud = true;
			else if(arg == "--input-hud") options.InputHud = true;
			else if(arg == "--compression") options.CompressionLevel = ParseNumber(take(i, arg));
			else if(!arg.empty() && arg[0] == '-') UsageError("unknown option: " + arg);
			else if(options.Rom.empty()) options.Rom = arg;
			else UsageError("only one ROM may be loaded per invocation");
		}

		if(options.Rom.empty()) UsageError("a ROM path is required");
		if(options.TimeoutSeconds == 0) UsageError("timeout must be at least one second");
		if(options.LuaTimeoutSeconds == 0) UsageError("Lua timeout must be at least one second");
		if(options.LuaTimeoutSeconds > options.TimeoutSeconds) UsageError("Lua timeout cannot exceed the overall timeout");
		if(options.CompressionLevel > 9) UsageError("compression must be between 0 and 9");
		return options;
	}

	CpuType SelectCpu(const Options& options)
	{
		if(options.Cpu.empty()) return AgentGetPrimaryCpuType();
		auto cpu = ParseEnum<CpuType>(options.Cpu);
		if(!cpu) UsageError("unknown CPU type: " + options.Cpu);
		return *cpu;
	}

	void EnableDebuggerForCpu(CpuType cpu)
	{
		DebuggerFlags flag;
		switch(cpu) {
			case CpuType::Snes: flag = DebuggerFlags::SnesDebuggerEnabled; break;
			case CpuType::Spc: flag = DebuggerFlags::SpcDebuggerEnabled; break;
			case CpuType::NecDsp: flag = DebuggerFlags::NecDspDebuggerEnabled; break;
			case CpuType::Sa1: flag = DebuggerFlags::Sa1DebuggerEnabled; break;
			case CpuType::Gsu: flag = DebuggerFlags::GsuDebuggerEnabled; break;
			case CpuType::Cx4: flag = DebuggerFlags::Cx4DebuggerEnabled; break;
			case CpuType::St018: flag = DebuggerFlags::St018DebuggerEnabled; break;
			case CpuType::Gameboy: flag = DebuggerFlags::GbDebuggerEnabled; break;
			case CpuType::Nes: flag = DebuggerFlags::NesDebuggerEnabled; break;
			case CpuType::Pce: flag = DebuggerFlags::PceDebuggerEnabled; break;
			case CpuType::Sms: flag = DebuggerFlags::SmsDebuggerEnabled; break;
			case CpuType::Gba: flag = DebuggerFlags::GbaDebuggerEnabled; break;
			case CpuType::Ws: flag = DebuggerFlags::WsDebuggerEnabled; break;
		}
		SetDebuggerFlag(flag, true);
	}

	StepType SelectStepType(const string& name)
	{
		if(name == "step") return StepType::Step;
		if(name == "over") return StepType::StepOver;
		if(name == "out") return StepType::StepOut;
		if(name == "cycle") return StepType::CpuCycleStep;
		if(name == "ppu") return StepType::PpuStep;
		if(name == "scanline") return StepType::PpuScanline;
		if(name == "frame") return StepType::PpuFrame;
		if(name == "nmi") return StepType::RunToNmi;
		if(name == "irq") return StepType::RunToIrq;
		if(name == "back") return StepType::StepBack;
		UsageError("unknown step type: " + name);
	}

	VideoCodec SelectVideoCodec(const Options& options)
	{
		string codec = options.VideoCodecName;
		if(codec.empty() && Lower(fs::u8path(options.Video).extension().u8string()) == ".gif") codec = "gif";
		if(codec.empty() || codec == "cscd") return VideoCodec::CSCD;
		if(codec == "zmbv") return VideoCodec::ZMBV;
		if(codec == "gif") return VideoCodec::GIF;
		UsageError("unknown video codec: " + codec);
	}

	CliBreakpoint ParseBreakpoint(const string& spec, CpuType cpu, uint32_t id)
	{
		vector<string> fields = Split(spec, ':');
		if(fields.size() < 3) UsageError("breakpoint syntax is TYPE:START[-END]:rwx[:CONDITION]");
		CliBreakpoint bp = {};
		bp.Id = id;
		bp.Cpu = cpu;
		bp.Memory = ParseMemoryType(fields[0], cpu);
		size_t dash = fields[1].find('-');
		bp.Start = (int32_t)ParseNumber(dash == string::npos ? fields[1] : fields[1].substr(0, dash));
		bp.End = dash == string::npos ? bp.Start : (int32_t)ParseNumber(fields[1].substr(dash + 1));
		if(bp.End < bp.Start) UsageError("breakpoint end precedes start");
		uint8_t flags = 0;
		for(char c : Lower(fields[2])) {
			if(c == 'r') flags |= (uint8_t)BreakpointTypeFlags::Read;
			else if(c == 'w') flags |= (uint8_t)BreakpointTypeFlags::Write;
			else if(c == 'x') flags |= (uint8_t)BreakpointTypeFlags::Execute;
			else UsageError("breakpoint access must use r, w and/or x");
		}
		if(flags == 0) UsageError("breakpoint requires at least one access flag");
		bp.Type = (BreakpointTypeFlags)flags;
		bp.Enabled = true;
		bp.IgnoreDummyOperations = true;
		if(fields.size() > 3) {
			string condition = fields[3];
			for(size_t i = 4; i < fields.size(); i++) condition += ":" + fields[i];
			if(condition.size() >= sizeof(bp.Condition)) UsageError("breakpoint condition is too long");
			memcpy(bp.Condition, condition.data(), condition.size());
		}
		return bp;
	}

	uint32_t ResolveLength(const MemoryRequest& request, MemoryType type)
	{
		uint32_t size = GetMemorySize(type);
		if(request.Start > size) throw runtime_error("memory start is outside " + request.Type);
		uint32_t length = request.Length.value_or(size - request.Start);
		if(length > size - request.Start) throw runtime_error("memory range is outside " + request.Type);
		return length;
	}

	class Session
	{
	public:
		bool Active = false;
		bool TraceActive = false;
		~Session()
		{
			if(TraceActive) StopLogTraceToFile();
			if(AviIsRecording()) AviStop();
			if(WaveIsRecording()) WaveStop();
			if(Active) {
				Stop();
				Release();
			}
		}
	};

	int Run(const Options& options)
	{
		fs::create_directories(fs::u8path(options.Home));
		Session session;
		InitDll();
		session.Active = true;
		InitializeEmu(options.Home.c_str(), nullptr, nullptr, true, true, true, true);
		AgentInitializeHeadlessDefaults();
		Pause();
		SetEmulationFlag(EmulationFlags::ConsoleMode, true);
		SetEmulationFlag(EmulationFlags::MaximumSpeed, options.MaximumSpeed);

		string rom = fs::absolute(fs::u8path(options.Rom)).u8string();
		string patch = options.Patch.empty() ? "" : fs::absolute(fs::u8path(options.Patch)).u8string();
		vector<char> romArg(rom.begin(), rom.end()); romArg.push_back(0);
		vector<char> patchArg(patch.begin(), patch.end()); patchArg.push_back(0);
		if(!LoadRom(romArg.data(), patch.empty() ? nullptr : patchArg.data())) {
			throw runtime_error("Mesen could not load ROM: " + rom);
		}

		InitializeDebugger();
		CpuType cpu = SelectCpu(options);
		ConsoleType console = AgentGetConsoleType();
		EnableDebuggerForCpu(cpu);
		// Loading a ROM that was paused before the debugger existed schedules an
		// asynchronous one-instruction debugger break.  Wait for that break before
		// mutating debugger-owned state (scripts, breakpoints, input schedules, etc.).
		// Otherwise a maximum-speed emulation thread can race the CLI setup and run
		// past a short input window before the frame-step request is installed.
		auto initialPauseDeadline = chrono::steady_clock::now() + chrono::seconds(options.TimeoutSeconds);
		while(IsRunning() && !IsExecutionStopped() && chrono::steady_clock::now() < initialPauseDeadline) {
			this_thread::sleep_for(chrono::milliseconds(1));
		}
		if(!IsRunning()) throw runtime_error("emulation stopped while waiting for the initial debugger break");
		if(!IsExecutionStopped()) throw runtime_error("timed out waiting for the initial debugger break");
		AgentSetLuaAccess(options.LuaAllowIo, options.LuaAllowNetwork, options.LuaTimeoutSeconds);
		array<char, 100> sha1 = {};
		GetRomHash(HashType::Sha1, sha1.data(), (uint32_t)sha1.size());
		uint32_t version = GetMesenVersion();
		string versionText = to_string(version >> 16) + "." + to_string((version >> 8) & 0xFF) + "." + to_string(version & 0xFF);
		Event("\"event\":\"loaded\",\"rom\":\"" + JsonEscape(rom) + "\",\"sha1\":\"" + JsonEscape(sha1.data()) + "\",\"mesen_version\":\"" + versionText + "\",\"console\":\"" + string(magic_enum::enum_name(console)) + "\",\"cpu\":\"" + string(magic_enum::enum_name(cpu)) + "\"");

		if(!options.LoadState.empty()) {
			string filename = fs::absolute(fs::u8path(options.LoadState)).u8string();
			if(!fs::is_regular_file(fs::u8path(filename))) throw runtime_error("save state does not exist: " + filename);
			vector<char> value(filename.begin(), filename.end()); value.push_back(0);
			LoadStateFile(value.data());
			Event("\"event\":\"state_loaded\",\"file\":\"" + JsonEscape(filename) + "\"");
		}

		vector<int32_t> scriptIds;
		for(const string& scriptFile : options.LuaScripts) {
			fs::path path = fs::absolute(fs::u8path(scriptFile));
			string content = ReadTextFile(path.u8string());
			string name = path.filename().u8string();
			string folder = path.parent_path().u8string();
			vector<char> nameArg(name.begin(), name.end()); nameArg.push_back(0);
			vector<char> folderArg(folder.begin(), folder.end()); folderArg.push_back(0);
			vector<char> contentArg(content.begin(), content.end()); contentArg.push_back(0);
			int32_t id = LoadScript(nameArg.data(), folderArg.data(), contentArg.data(), -1);
			if(id < 0) throw runtime_error("could not load Lua script: " + path.u8string());
			scriptIds.push_back(id);
			uint32_t errorCount = GetScriptErrorCount(id);
			if(errorCount > 0) {
				Event("\"event\":\"lua_error\",\"phase\":\"load\",\"id\":" + to_string(id) + ",\"file\":\"" + JsonEscape(path.u8string()) + "\",\"error_count\":" + to_string(errorCount));
				EmitScriptLog(id, scriptFile, errorCount);
				uint32_t frame = AgentGetFrameCount();
				Event("\"event\":\"summary\",\"ok\":false,\"reason\":\"lua_error\",\"start_frame\":" + to_string(frame) + ",\"final_frame\":" + to_string(frame) + ",\"frames_executed\":0,\"stop_code\":0");
				return 5;
			}
			Event("\"event\":\"lua_loaded\",\"id\":" + to_string(id) + ",\"file\":\"" + JsonEscape(path.u8string()) + "\"");
		}

		vector<CliBreakpoint> breakpoints;
		for(size_t i = 0; i < options.Breakpoints.size(); i++) breakpoints.push_back(ParseBreakpoint(options.Breakpoints[i], cpu, (uint32_t)i));
		if(!breakpoints.empty()) {
			SetBreakpoints(reinterpret_cast<Breakpoint*>(breakpoints.data()), (uint32_t)breakpoints.size());
		}

		for(const WriteRequest& write : options.Writes) {
			MemoryType type = ParseMemoryType(write.Type, cpu);
			uint32_t size = GetMemorySize(type);
			if(write.Start > size || write.Data.size() > size - write.Start) throw runtime_error("memory write is outside " + write.Type);
			SetMemoryValues(type, write.Start, const_cast<uint8_t*>(write.Data.data()), (int32_t)write.Data.size());
			Event("\"event\":\"memory_written\",\"type\":\"" + string(magic_enum::enum_name(type)) + "\",\"start\":" + to_string(write.Start) + ",\"length\":" + to_string(write.Data.size()));
		}

		if(options.SetPc) {
			SetProgramCounter(cpu, *options.SetPc);
			Event("\"event\":\"pc_set\",\"cpu\":\"" + string(magic_enum::enum_name(cpu)) + "\",\"value\":" + to_string(*options.SetPc));
		}

		if(!options.Trace.empty()) {
			EnsureParent(options.Trace);
			TraceLoggerOptions traceOptions = {};
			traceOptions.Enabled = true;
			memcpy(traceOptions.Format, "[Disassembly]", 14);
			SetTraceOptions(cpu, traceOptions);
			StartLogTraceToFile(options.Trace.c_str());
			session.TraceActive = true;
		}
		if(!options.Video.empty()) {
			EnsureParent(options.Video);
			RecordAviOptions recordOptions = { SelectVideoCodec(options), options.CompressionLevel, options.VideoHud, options.InputHud };
			string filename = fs::absolute(fs::u8path(options.Video)).u8string();
			vector<char> value(filename.begin(), filename.end()); value.push_back(0);
			AviRecord(value.data(), recordOptions);
			if(!AviIsRecording()) throw runtime_error("could not start video recording: " + filename);
		}
		if(!options.Audio.empty()) {
			EnsureParent(options.Audio);
			string filename = fs::absolute(fs::u8path(options.Audio)).u8string();
			vector<char> value(filename.begin(), filename.end()); value.push_back(0);
			WaveRecord(value.data());
			if(!WaveIsRecording()) throw runtime_error("could not start audio recording: " + filename);
		}

		uint32_t startFrame = AgentGetFrameCount();
		SetInputSchedules(options.Inputs.empty() ? nullptr : const_cast<DebugInputSchedule*>(options.Inputs.data()), (uint32_t)options.Inputs.size(), startFrame);
		bool timedOut = false;
		bool executionStopped = false;
		bool emulationStopped = false;
		uint32_t lastFrame = startFrame;
		auto deadline = chrono::steady_clock::now() + chrono::seconds(options.TimeoutSeconds);
		uint64_t runStartClock = AgentGetMasterClock();
		if(options.Frames > 0) {
			//Use the core debugger's PPU-frame counter as the run boundary.  Host-side
			//polling can overshoot on fast machines and makes Lua event timing depend
			//on scheduler latency; a PPU-frame step stops at the exact core boundary.
			//Step also releases the current debugger break.  Calling Resume first would
			//open a race where the core can execute frames before this request is armed.
			Step(cpu, options.Frames, StepType::PpuFrame);
		}
		while(options.Frames > 0) {
			uint32_t current = AgentGetFrameCount();
			if(!IsRunning()) { emulationStopped = true; break; }
			if(current >= startFrame) lastFrame = std::max(lastFrame, current);
			uint32_t relative = lastFrame - startFrame;
			bool stopped = IsExecutionStopped();
			if(stopped && (relative > 0 || AgentGetMasterClock() != runStartClock)) {
				//The PPU-frame request itself is the normal frame-limit stop.  Any
				//earlier stop came from a breakpoint or script debug request.
				executionStopped = relative < options.Frames;
				break;
			}
			if(chrono::steady_clock::now() >= deadline) { timedOut = true; break; }
			this_thread::sleep_for(chrono::milliseconds(1));
		}
		auto hasLuaErrors = [&]() {
			for(int32_t scriptId : scriptIds) {
				if(GetScriptErrorCount(scriptId) > 0) return true;
			}
			return false;
		};
		bool luaFailed = hasLuaErrors();
		if(options.StepCount > 0 && !emulationStopped && !timedOut && !luaFailed) {
			uint64_t stepStartClock = AgentGetMasterClock();
			//Debugger Step atomically installs the request and releases the current
			//break.  A separate Resume here would create the same run-before-step race.
			Step(cpu, options.StepCount, SelectStepType(options.StepTypeName));
			while(true) {
				if(!IsRunning()) { emulationStopped = true; break; }
				if(chrono::steady_clock::now() >= deadline) { timedOut = true; break; }
				bool stopped = IsExecutionStopped();
				//The running phase can be shorter than this thread's polling interval.
				//A changed master clock proves the requested step actually executed,
				//so a slow scheduler cannot mistake the initial break for completion.
				if(stopped && AgentGetMasterClock() != stepStartClock) break;
				this_thread::sleep_for(chrono::milliseconds(1));
			}
			executionStopped = IsExecutionStopped();
		} else if(!executionStopped && !emulationStopped) {
			Pause();
		}
		if(timedOut && !emulationStopped) Pause();
		this_thread::sleep_for(chrono::milliseconds(20));

		if(session.TraceActive) {
			StopLogTraceToFile();
			session.TraceActive = false;
			RequireArtifact(options.Trace, "trace", true);
			Event("\"event\":\"trace_saved\",\"file\":\"" + JsonEscape(fs::absolute(fs::u8path(options.Trace)).u8string()) + "\"");
		}
		if(!options.Video.empty()) {
			if(AviIsRecording()) AviStop();
			RequireArtifact(options.Video, "video");
			Event("\"event\":\"video_saved\",\"file\":\"" + JsonEscape(fs::absolute(fs::u8path(options.Video)).u8string()) + "\"");
		}
		if(!options.Audio.empty()) {
			if(WaveIsRecording()) WaveStop();
			RequireArtifact(options.Audio, "audio");
			Event("\"event\":\"audio_saved\",\"file\":\"" + JsonEscape(fs::absolute(fs::u8path(options.Audio)).u8string()) + "\"");
		}

		if(options.ListMemory) {
			for(auto entry : magic_enum::enum_entries<MemoryType>()) {
				if(entry.first == MemoryType::None) continue;
				uint32_t size = GetMemorySize(entry.first);
				if(size > 0) Event("\"event\":\"memory_space\",\"type\":\"" + string(entry.second) + "\",\"size\":" + to_string(size));
			}
		}

		for(const MemoryRequest& request : options.Reads) {
			MemoryType type = ParseMemoryType(request.Type, cpu);
			uint32_t length = ResolveLength(request, type);
			vector<uint8_t> data(length);
			if(length) GetMemoryValues(type, request.Start, request.Start + length - 1, data.data());
			Event("\"event\":\"memory_read\",\"type\":\"" + string(magic_enum::enum_name(type)) + "\",\"start\":" + to_string(request.Start) + ",\"length\":" + to_string(length) + ",\"hex\":\"" + Hex(data.data(), data.size()) + "\"");
		}

		for(const MemoryRequest& request : options.Dumps) {
			MemoryType type = ParseMemoryType(request.Type, cpu);
			uint32_t length = ResolveLength(request, type);
			vector<uint8_t> data(length);
			if(length) GetMemoryValues(type, request.Start, request.Start + length - 1, data.data());
			WriteBinaryFile(request.File, data);
			Event("\"event\":\"memory_dumped\",\"type\":\"" + string(magic_enum::enum_name(type)) + "\",\"start\":" + to_string(request.Start) + ",\"length\":" + to_string(length) + ",\"file\":\"" + JsonEscape(fs::absolute(fs::u8path(request.File)).u8string()) + "\"");
		}

		for(const string& expression : options.Expressions) {
			EvalResultType resultType = EvalResultType::Invalid;
			int64_t value = EvaluateExpression(expression.c_str(), cpu, &resultType, false);
			Event("\"event\":\"expression\",\"cpu\":\"" + string(magic_enum::enum_name(cpu)) + "\",\"expression\":\"" + JsonEscape(expression) + "\",\"result_type\":\"" + string(magic_enum::enum_name(resultType)) + "\",\"value\":" + to_string(value));
		}

		for(const DisassembleRequest& request : options.Disassembly) {
			//The desktop API includes labels and collapsible block-marker rows in
			//its row count.  JSONL callers ask for instructions, so fetch enough UI
			//rows to cover those markers and emit only rows with decoded opcodes.
			uint64_t requestedRows = (uint64_t)request.Lines * 3 + 16;
			if(requestedRows > 1000000) UsageError("disassembly line count is too large");
			vector<CodeLineData> lines((size_t)requestedRows);
			uint32_t count = GetDisassemblyOutput(cpu, request.Address, lines.data(), (uint32_t)lines.size());
			uint32_t emitted = 0;
			for(uint32_t i = 0; i < count && emitted < request.Lines; i++) {
				CodeLineData& line = lines[i];
				if(line.OpSize == 0) continue;
				Event("\"event\":\"disassembly\",\"cpu\":\"" + string(magic_enum::enum_name(cpu)) + "\",\"address\":" + to_string(line.Address) + ",\"absolute_type\":\"" + string(magic_enum::enum_name(line.AbsoluteAddress.Type)) + "\",\"absolute_address\":" + to_string(line.AbsoluteAddress.Address) + ",\"bytes\":\"" + Hex(line.ByteCode, line.OpSize) + "\",\"text\":\"" + JsonEscape(line.Text) + "\",\"comment\":\"" + JsonEscape(line.Comment) + "\"");
				emitted++;
			}
		}

		if(options.ShowPc) {
			uint32_t pc = GetProgramCounter(cpu, true);
			Event("\"event\":\"program_counter\",\"cpu\":\"" + string(magic_enum::enum_name(cpu)) + "\",\"value\":" + to_string(pc));
		}

		if(options.ShowCallstack) {
			uint32_t size = 512;
			vector<StackFrameInfo> stack(size);
			GetCallstack(cpu, stack.data(), size);
			for(uint32_t i = 0; i < size; i++) {
				const StackFrameInfo& frame = stack[i];
				Event("\"event\":\"callstack\",\"index\":" + to_string(i) + ",\"source\":" + to_string(frame.Source) + ",\"target\":" + to_string(frame.Target) + ",\"return\":" + to_string(frame.Return));
			}
		}

		for(const auto& cdl : options.CdlFiles) {
			MemoryType type = ParseMemoryType(cdl.first, cpu);
			EnsureParent(cdl.second);
			string filename = fs::absolute(fs::u8path(cdl.second)).u8string();
			vector<char> value(filename.begin(), filename.end()); value.push_back(0);
			SaveCdlFile(type, value.data());
			RequireArtifact(filename, "CDL");
			Event("\"event\":\"cdl_saved\",\"type\":\"" + string(magic_enum::enum_name(type)) + "\",\"file\":\"" + JsonEscape(filename) + "\"");
		}

		if(!options.SaveState.empty()) {
			EnsureParent(options.SaveState);
			string filename = fs::absolute(fs::u8path(options.SaveState)).u8string();
			vector<char> value(filename.begin(), filename.end()); value.push_back(0);
			SaveStateFile(value.data());
			RequireArtifact(filename, "save state");
			Event("\"event\":\"state_saved\",\"file\":\"" + JsonEscape(filename) + "\"");
		}
		if(!options.StateJson.empty()) {
			EnsureParent(options.StateJson);
			string filename = fs::absolute(fs::u8path(options.StateJson)).u8string();
			if(!AgentSaveStateJson(filename.c_str())) throw runtime_error("could not save state JSON: " + filename);
			Event("\"event\":\"state_json_saved\",\"file\":\"" + JsonEscape(filename) + "\"");
		}
		if(!options.Screenshot.empty()) {
			EnsureParent(options.Screenshot);
			string filename = fs::absolute(fs::u8path(options.Screenshot)).u8string();
			if(!AgentSaveScreenshot(filename.c_str())) throw runtime_error("could not save screenshot (no decoded frame is available): " + filename);
			Event("\"event\":\"screenshot_saved\",\"file\":\"" + JsonEscape(filename) + "\"");
		}

		if(!emulationStopped && !timedOut) {
			for(int32_t scriptId : scriptIds) {
				RemoveScript(scriptId);
			}
			if(GetStopCode() != 0) {
				emulationStopped = true;
			}
		}

		luaFailed = hasLuaErrors();
		for(size_t i = 0; i < scriptIds.size(); i++) {
			uint32_t errorCount = GetScriptErrorCount(scriptIds[i]);
			if(errorCount > 0) {
				Event("\"event\":\"lua_error\",\"phase\":\"callback\",\"id\":" + to_string(scriptIds[i]) + ",\"file\":\"" + JsonEscape(fs::absolute(fs::u8path(options.LuaScripts[i])).u8string()) + "\",\"error_count\":" + to_string(errorCount));
			}
			EmitScriptLog(scriptIds[i], options.LuaScripts[i], errorCount);
		}

		if(!options.Log.empty()) {
			vector<char> log(1000000, 0);
			GetLog(log.data(), (uint32_t)log.size());
			EnsureParent(options.Log);
			std::ofstream output(fs::u8path(options.Log), ios::binary | ios::trunc);
			output.write(log.data(), strlen(log.data()));
			if(!output.good()) throw runtime_error("could not save log: " + options.Log);
			Event("\"event\":\"log_saved\",\"file\":\"" + JsonEscape(fs::absolute(fs::u8path(options.Log)).u8string()) + "\"");
		}

		uint32_t finalFrame = AgentGetFrameCount();
		if(finalFrame < startFrame) finalFrame = lastFrame;
		int32_t stopCode = GetStopCode();
		string reason = luaFailed ? "lua_error" : timedOut ? "timeout" : emulationStopped ? "emulation_stopped" : executionStopped ? "debugger_break" : "frame_limit";
		Event("\"event\":\"summary\",\"ok\":" + string(timedOut || luaFailed ? "false" : "true") + ",\"reason\":\"" + reason + "\",\"start_frame\":" + to_string(startFrame) + ",\"final_frame\":" + to_string(finalFrame) + ",\"frames_executed\":" + to_string(finalFrame - startFrame) + ",\"stop_code\":" + to_string(stopCode));
		if(luaFailed) return 5;
		if(timedOut) return 4;
		if(emulationStopped) return stopCode;
		return 0;
	}
}

int main(int argc, char** argv)
{
	try {
		return Run(ParseArguments(argc, argv));
	} catch(const invalid_argument& error) {
		cerr << "{\"event\":\"error\",\"kind\":\"usage\",\"message\":\"" << JsonEscape(error.what()) << "\"}" << endl;
		cerr << "Run with --help for usage." << endl;
		return 2;
	} catch(const exception& error) {
		cerr << "{\"event\":\"error\",\"kind\":\"runtime\",\"message\":\"" << JsonEscape(error.what()) << "\"}" << endl;
		return 3;
	}
}
