#include "pch.h"
#include "Debugger/ScriptManager.h"
#include "Debugger/ScriptHost.h"
#include "Debugger/DebugBreakHelper.h"
#include "Debugger/Debugger.h"
#include "Shared/Emulator.h"
#include "Shared/Video/DebugHud.h"
#include "Shared/MemoryOperationType.h"

unordered_map<int32_t, string> ScriptManager::_archivedLogs;
unordered_map<int32_t, uint32_t> ScriptManager::_archivedErrorCounts;
SimpleLock ScriptManager::_archivedLogLock;

ScriptManager::ScriptManager(Debugger* debugger)
{
	_debugger = debugger;
	_hasScript = false;
	_nextScriptId = 1;
}

ScriptManager::~ScriptManager()
{
	{
		auto lock = _archivedLogLock.AcquireSafe();
		for(unique_ptr<ScriptHost>& script : _scripts) {
			_archivedLogs[script->GetScriptId()] = script->GetLog();
			_archivedErrorCounts[script->GetScriptId()] = script->GetErrorCount();
		}
	}
	_debugger->GetEmulator()->GetDebugHud()->ClearScreen();
	_debugger->GetEmulator()->GetScriptHud()->ClearScreen();
}

int ScriptManager::LoadScript(string name, string path, string content, int32_t scriptId)
{
	DebugBreakHelper helper(_debugger);
	auto lock = _scriptLock.AcquireSafe();

	if(scriptId < 0) {
		unique_ptr<ScriptHost> script(new ScriptHost(_nextScriptId++));
		script->LoadScript(name, path, content, _debugger);
		scriptId = script->GetScriptId();
		{
			auto archivedLock = _archivedLogLock.AcquireSafe();
			_archivedLogs.erase(scriptId);
			_archivedErrorCounts.erase(scriptId);
		}
		_scripts.push_back(std::move(script));
		_hasScript = true;
		return scriptId;
	} else {
		auto result = std::find_if(_scripts.begin(), _scripts.end(), [=](unique_ptr<ScriptHost> &script) {
			return script->GetScriptId() == scriptId;
		});
		if(result != _scripts.end()) {
			//Send a ScriptEnded event before reloading the code
			(*result)->ProcessEvent(EventType::ScriptEnded, _debugger->GetMainCpuType());

			(*result)->LoadScript(name, path, content, _debugger);
			RefreshMemoryCallbackFlags();
			return scriptId;
		}
	}

	return -1;
}

void ScriptManager::RemoveScript(int32_t scriptId)
{
	DebugBreakHelper helper(_debugger);
	auto lock = _scriptLock.AcquireSafe();
	_scripts.erase(std::remove_if(_scripts.begin(), _scripts.end(), [=](const unique_ptr<ScriptHost>& script) {
		if(script->GetScriptId() == scriptId) {
			//Send a ScriptEnded event before unloading the script
			script->ProcessEvent(EventType::ScriptEnded, _debugger->GetMainCpuType());
			{
				auto archivedLock = _archivedLogLock.AcquireSafe();
				_archivedLogs[scriptId] = script->GetLog();
				_archivedErrorCounts[scriptId] = script->GetErrorCount();
			}
			_debugger->GetEmulator()->GetDebugHud()->ClearScreen();
			_debugger->GetEmulator()->GetScriptHud()->ClearScreen();
			return true;
		}
		return false;
	}), _scripts.end());

	RefreshMemoryCallbackFlags();

	_hasScript = _scripts.size() > 0;
}

void ScriptManager::RefreshMemoryCallbackFlags()
{
	_isPpuMemoryCallbackEnabled = false;
	_isCpuMemoryCallbackEnabled = false;
	for(unique_ptr<ScriptHost>& script : _scripts) {
		script->RefreshMemoryCallbackFlags();
	}
}

string ScriptManager::GetScriptLog(int32_t scriptId)
{
	auto lock = _scriptLock.AcquireSafe();
	for(unique_ptr<ScriptHost> &script : _scripts) {
		if(script->GetScriptId() == scriptId) {
			return script->GetLog();
		}
	}
	return "";
}

string ScriptManager::GetArchivedScriptLog(int32_t scriptId)
{
	auto lock = _archivedLogLock.AcquireSafe();
	auto result = _archivedLogs.find(scriptId);
	return result == _archivedLogs.end() ? "" : result->second;
}

uint32_t ScriptManager::GetScriptErrorCount(int32_t scriptId)
{
	auto lock = _scriptLock.AcquireSafe();
	for(unique_ptr<ScriptHost>& script : _scripts) {
		if(script->GetScriptId() == scriptId) {
			return script->GetErrorCount();
		}
	}
	return 0;
}

uint32_t ScriptManager::GetArchivedScriptErrorCount(int32_t scriptId)
{
	auto lock = _archivedLogLock.AcquireSafe();
	auto result = _archivedErrorCounts.find(scriptId);
	return result == _archivedErrorCounts.end() ? 0 : result->second;
}

void ScriptManager::ProcessEvent(EventType type, CpuType cpuType)
{
	for(unique_ptr<ScriptHost> &script : _scripts) {
		script->ProcessEvent(type, cpuType);
	}
}
