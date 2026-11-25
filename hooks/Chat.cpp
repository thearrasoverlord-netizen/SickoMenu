#include "pch-il2cpp.h"
#include "_hooks.h"
#include "_rpc.h"
#include "utility.h"
#include "game.h"
#include "state.hpp"
#include "toasts.hpp"
#include "console.hpp"
#include <regex>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <limits>
#include <deque>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <winhttp.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static float copyNotificationTimer = 0.f;
static bool isTextCut = false;

std::string trim(const std::string& str) {
	const auto start = str.find_first_not_of(" \t\n\r");
	if (start == std::string::npos) return "";
	const auto end = str.find_last_not_of(" \t\n\r");
	return str.substr(start, end - start + 1);
}

static std::string strToLower(std::string str) {
	std::string new_str = "";
	for (auto i : str) {
		new_str += char(std::tolower(i));
	}
	return new_str;
}


// TheSickMenu v2 social chat commands (/startpoll, /poll, /cancelpoll,
// /clearpolls, /pollqueue and /8ball). Poll state is authoritative on the host.
struct SickoPoll {
	std::string question;
	std::vector<std::string> options;
	std::string creatorName;
	int durationSeconds = 60;
	std::chrono::steady_clock::time_point endsAt{};
	std::unordered_map<uint32_t, size_t> votes; // IDs are private; never displayed.
};

static std::optional<SickoPoll> sickoActivePoll;
static std::deque<SickoPoll> sickoPollQueue;
static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> sickoLastPollVote;
static std::deque<std::string> sickoPollOutputQueue;
static std::deque<std::string> sickoPollSentMessages;
static float sickoPollOutputCooldown = 0.f;

static std::string SickoSanitizePollText(std::string value) {
	std::string clean;
	clean.reserve(value.size());
	for (char c : value) {
		if (c == '<') clean += '[';
		else if (c == '>') clean += ']';
		else if (c == '\n' || c == '\r') clean += ' ';
		else if (static_cast<unsigned char>(c) >= 32) clean += c;
	}
	return trim(clean);
}

static std::string SickoShorten(std::string value, size_t maxLength) {
	if (value.size() <= maxLength) return value;
	if (maxLength <= 3) return value.substr(0, maxLength);
	size_t limit = maxLength - 3;
	while (limit > 0 && (static_cast<unsigned char>(value[limit]) & 0xC0) == 0x80)
		--limit; // Never cut a UTF-8 character in half.
	return value.substr(0, limit) + "...";
}

static bool IsSickoSocialCommand(const std::string& message) {
	if (message.empty() || message[0] != '/') return false;
	size_t end = message.find_first_of(" \t");
	std::string command = strToLower(message.substr(0, end));
	static const std::set<std::string> commands = {
		"/startpoll", "/poll", "/cancelpoll", "/clearpolls", "/pollqueue", "/8ball", "/tl", "/rnd", "/selfinf", "/ai"
	};
	return commands.count(command) != 0;
}

static bool IsSicko8BallCommand(const std::string& message) {
	if (message.empty() || message[0] != '/') return false;
	size_t end = message.find_first_of(" \t");
	return strToLower(message.substr(0, end)) == "/8ball";
}

static void QueueSickoPollMessage(const std::string& message) {
	if (!message.empty()) sickoPollOutputQueue.push_back(SickoShorten(message, 120));
}

static void QueueSickoPollMessages(const std::vector<std::string>& messages, bool replacePending) {
	if (replacePending) sickoPollOutputQueue.clear();

	// One logical update = one SendChat RPC. Newlines keep the report together.
	std::string combined;
	for (size_t i = 0; i < messages.size(); ++i) {
		if (messages[i].empty()) continue;
		if (!combined.empty()) combined += (i == 1 ? "\n\n" : "\n");
		combined += messages[i];
	}
	if (!combined.empty()) QueueSickoPollMessage(combined);
}

static std::vector<std::string> BuildSickoPollReport(const SickoPoll& poll, bool cancelled, bool live) {
	std::vector<int> counts(poll.options.size(), 0);
	for (const auto& vote : poll.votes) {
		if (vote.second < counts.size()) ++counts[vote.second];
	}
	const int totalVotes = static_cast<int>(poll.votes.size());
	const int highest = counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
	std::vector<std::string> messages;
	const std::string heading = cancelled ? "🚫 Cancelada" :
		(live ? "📊 En vivo" : "📊 Final");
	messages.push_back(std::format("{} ({}): {}", heading, totalVotes, SickoShorten(poll.question, 18)));

	for (size_t i = 0; i < poll.options.size(); ++i) {
		const int percentage = totalVotes == 0 ? 0 :
			static_cast<int>(std::lround((static_cast<double>(counts[i]) * 100.0) / totalVotes));
		const bool winner = totalVotes > 0 && counts[i] == highest;
		const char letter = static_cast<char>('A' + i);
		messages.push_back(std::format("{}:{} {}v/{}%{}",
			letter, SickoShorten(poll.options[i], 7), counts[i], percentage, winner ? "*" : ""));
	}
	if (totalVotes == 0) messages.push_back("Sin votos.");
	return messages;
}

static void StartNextSickoPoll() {
	if (sickoActivePoll.has_value() || sickoPollQueue.empty()) return;
	sickoActivePoll = std::move(sickoPollQueue.front());
	sickoPollQueue.pop_front();
	sickoActivePoll->endsAt = std::chrono::steady_clock::now() +
		std::chrono::seconds(sickoActivePoll->durationSeconds);
	QueueSickoPollMessage(std::format("📊 Nueva encuesta: {} ({} s)",
		SickoShorten(sickoActivePoll->question, 78), sickoActivePoll->durationSeconds));
}

static void FinishSickoPoll(bool cancelled) {
	if (!sickoActivePoll.has_value()) return;
	SickoPoll finished = std::move(*sickoActivePoll);
	sickoActivePoll.reset();
	QueueSickoPollMessages(BuildSickoPollReport(finished, cancelled, false), true);
	StartNextSickoPoll();
}

static void QueueSickoLiveResults() {
	if (sickoActivePoll.has_value())		QueueSickoPollMessages(BuildSickoPollReport(*sickoActivePoll, false, true), true);
}

void ResetSickoPollState() {
	sickoActivePoll.reset();
	sickoPollQueue.clear();
	sickoLastPollVote.clear();
	sickoPollOutputQueue.clear();
	sickoPollSentMessages.clear();
	sickoPollOutputCooldown = 0.f;
}

static void UpdateSickoPolls() {
	// Only the host owns poll state. Leaving the lobby/game discards it.
	if (!IsHost()) {
		ResetSickoPollState();
		return;
	}

	if (sickoActivePoll.has_value() &&
		std::chrono::steady_clock::now() >= sickoActivePoll->endsAt) {
		FinishSickoPoll(false);
	}
	if (!sickoActivePoll.has_value() && !sickoPollQueue.empty())
		StartNextSickoPoll();

	if (sickoPollOutputCooldown > 0.f)
		sickoPollOutputCooldown -= Time_get_deltaTime(NULL);

	if (sickoPollOutputCooldown <= 0.f && !sickoPollOutputQueue.empty() &&
		Game::pLocalPlayer != nullptr && *Game::pLocalPlayer != nullptr &&
		(IsInLobby() || IsInGame())) {
		const std::string message = sickoPollOutputQueue.front();
		sickoPollOutputQueue.pop_front();
		sickoPollSentMessages.push_back(message);
		while (sickoPollSentMessages.size() > 16) sickoPollSentMessages.pop_front();
		PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(message), NULL);
		sickoPollOutputCooldown = 3.2f;
	}
}

static bool ParseSickoQuotedArguments(const std::string& input,
	std::vector<std::string>& quoted, std::string& trailing) {
	size_t i = 0;
	auto skipSeparators = [&]() {
		while (i < input.size() && (std::isspace(static_cast<unsigned char>(input[i])) || input[i] == ',')) ++i;
	};

	skipSeparators();
	while (i < input.size() && input[i] == '"') {
		++i;
		std::string value;
		bool closed = false;
		while (i < input.size()) {
			if (input[i] == '\\' && i + 1 < input.size() && input[i + 1] == '"') {
				value += '"';
				i += 2;
			}
			else if (input[i] == '"') {
				++i;
				closed = true;
				break;
			}
			else {
				value += input[i++];
			}
		}
		if (!closed) return false;
		quoted.push_back(value);
		skipSeparators();
		if (i < input.size() && input[i] != '"') {
			trailing = trim(input.substr(i));
			break;
		}
	}
	return !quoted.empty();
}

static bool ParseSickoDuration(const std::string& value, int& duration) {
	if (value.empty()) return false;
	try {
		size_t used = 0;
		int parsed = std::stoi(value, &used);
		if (used != value.size()) return false;
		duration = parsed;
		return true;
	}
	catch (...) {
		return false;
	}
}



// Extra local utility commands. Translation runs off-thread; chat RPCs are
// queued and sent from ChatController_Update after the HTTP response arrives.
static std::mutex sickoUtilityMutex;
static std::deque<std::string> sickoUtilityOutputQueue;

static std::string SickoUrlEncode(const std::string& value) {
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char ch : value) {
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += static_cast<char>(ch);
		else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
	}
	return out;
}

static bool SickoContainsEmoji(const std::string& text) {
	for (size_t i = 0; i < text.size();) {
		uint32_t cp = static_cast<unsigned char>(text[i++]);
		if (cp >= 0xF0 && i + 2 < text.size()) {
			cp = ((cp & 7) << 18) | ((static_cast<unsigned char>(text[i]) & 0x3F) << 12) |
				((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(text[i + 2]) & 0x3F);
			i += 3;
		}
		else if (cp >= 0xE0 && i + 1 < text.size()) {
			cp = ((cp & 15) << 12) | ((static_cast<unsigned char>(text[i]) & 0x3F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
			i += 2;
		}
		else if (cp >= 0xC0 && i < text.size()) {
			cp = ((cp & 31) << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
			++i;
		}
		if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
			(cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D || cp == 0x20E3 ||
			(cp >= 0x1F1E6 && cp <= 0x1F1FF)) return true;
	}
	return false;
}

static std::string SickoJsonString(const std::string& value) {
	std::string out = "\"";
	for (unsigned char ch : value) {
		switch (ch) {
		case '\"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: if (ch >= 32) out += static_cast<char>(ch); break;
		}
	}
	out += "\"";
	return out;
}

static bool SickoExtractJsonString(const std::string& json, const std::string& key, std::string& value) {
	const std::string token = "\"" + key + "\"";
	size_t pos = json.find(token);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + token.size());
	if (pos == std::string::npos) return false;
	pos = json.find('"', pos + 1);
	if (pos == std::string::npos) return false;
	++pos;	value.clear();
	while (pos < json.size()) {
		char ch = json[pos++];
		if (ch == '"') return true;
		if (ch == '\\' && pos < json.size()) {
			char escaped = json[pos++];
			switch (escaped) {
			case '"': value += '"'; break;
			case '\\': value += '\\'; break;
			case '/': value += '/'; break;
			case 'n': value += '\n'; break;
			case 'r': value += '\r'; break;
			case 't': value += '\t'; break;
			case 'b': value += '\b'; break;
			case 'f': value += '\f'; break;
			case 'u': {
				if (pos + 4 > json.size()) return false;
				uint32_t cp = 0;
				for (int n = 0; n < 4; ++n) {
					char h = json[pos++];
					cp <<= 4;
					if (h >= '0' && h <= '9') cp |= h - '0';
					else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
					else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
					else return false;
				}
				if (cp <= 0x7F) value += static_cast<char>(cp);
				else if (cp <= 0x7FF) {
					value += static_cast<char>(0xC0 | (cp >> 6));
					value += static_cast<char>(0x80 | (cp & 0x3F));
				}
				else {
					value += static_cast<char>(0xE0 | (cp >> 12));
					value += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
					value += static_cast<char>(0x80 | (cp & 0x3F));
				}
				break;
			}
			default: value += escaped; break;
			}
		}
		else value += ch;
	}
	return false;
}



// SMAI integration: asynchronous HTTPS requests; the bot token is read only
// from the local SMAI_BOT_TOKEN environment variable and is never committed.
struct SickoAiHistoryItem {
	std::string role;
	std::string content;
};
struct SickoAiOutput {
	std::string playerKey;
	std::string message;
	bool error = false;
};
static std::unordered_map<std::string, std::deque<SickoAiHistoryItem>> sickoAiHistory;
static std::mutex sickoAiMutex;
static std::deque<SickoAiOutput> sickoAiOutputQueue;
static std::atomic<int> sickoAiRequestsInFlight{ 0 };
static float sickoAiOutputCooldown = 0.f;

static std::string SickoGetFriendCode(PlayerControl* actor) {
	if (!actor) return "";
	auto data = GetPlayerData(actor);
	if (!data || !data->fields.FriendCode) return "";
	return trim(convert_from_string(data->fields.FriendCode));
}

static std::string SickoHmacPlayerId(const std::string& friendCode, const std::string& token) {
	if (friendCode.empty() || token.empty()) return "";
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hashHandle = nullptr;
	DWORD objectLength = 0, bytesCopied = 0;
	std::vector<UCHAR> hashObject;
	std::array<UCHAR, 32> hash{};
	std::string result;

	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
		BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return "";
	if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
		sizeof(objectLength), &bytesCopied, 0) < 0) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
		return "";
	}
	hashObject.resize(objectLength);
	if (BCryptCreateHash(algorithm, &hashHandle, hashObject.data(), objectLength,
		reinterpret_cast<PUCHAR>(const_cast<char*>(token.data())),
		static_cast<ULONG>(token.size()), 0) >= 0 &&
		BCryptHashData(hashHandle, reinterpret_cast<PUCHAR>(const_cast<char*>(friendCode.data())),
			static_cast<ULONG>(friendCode.size()), 0) >= 0 &&
		BCryptFinishHash(hashHandle, hash.data(), static_cast<ULONG>(hash.size()), 0) >= 0) {
		static const char hex[] = "0123456789abcdef";
		result = "p_";
		for (UCHAR byte : hash) {
			result += hex[byte >> 4];
			result += hex[byte & 15];
		}
	}
	if (hashHandle) BCryptDestroyHash(hashHandle);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return result;
}

static std::string SickoGetPlayerDevice(PlayerControl* actor) {
	auto client = (InnerNetClient*)(Game::pAmongUsClient != nullptr ? *Game::pAmongUsClient : nullptr);
	auto clientData = client && actor
		? app::InnerNetClient_GetClientFromCharacter(client, actor, nullptr) : nullptr;
	if (!clientData || !clientData->fields.PlatformData) return "Unknown";
	switch (clientData->fields.PlatformData->fields.Platform) {
	case Platforms__Enum::StandaloneEpicPC: return "Epic Games (PC)";
	case Platforms__Enum::StandaloneSteamPC: return "Steam (PC)";
	case Platforms__Enum::StandaloneMac: return "Mac";
	case Platforms__Enum::StandaloneWin10: return "Microsoft Store (PC)";
	case Platforms__Enum::StandaloneItch: return "itch.io (PC)";
	case Platforms__Enum::IPhone: return "iOS";
	case Platforms__Enum::Android: return "Android";
	case Platforms__Enum::Switch: return "Nintendo Switch";
	case Platforms__Enum::Xbox: return "Xbox";
	case Platforms__Enum::Playstation: return "PlayStation";
	default: return "Unknown";
	}
}

static void RecordSickoAiHistory(PlayerControl* actor, const std::string& message) {
	if (!actor || message.empty() || message[0] == '/' || message.rfind("(AI): ", 0) == 0) return;
	const std::string playerKey = SickoGetFriendCode(actor);
	if (playerKey.empty()) return;
	auto& history = sickoAiHistory[playerKey];
	history.push_back({ "user", SickoShorten(trim(message), 2000) });
	while (history.size() > 8) history.pop_front();
	size_t total = 0;
	for (const auto& item : history) total += item.content.size();
	while (!history.empty() && total > 6000) {
		total -= history.front().content.size();
		history.pop_front();
	}
}

static void QueueSickoAiOutput(const std::string& playerKey, const std::string& message, bool error) {
	std::lock_guard<std::mutex> lock(sickoAiMutex);
	if (sickoAiOutputQueue.size() >= 8) sickoAiOutputQueue.pop_front();
	sickoAiOutputQueue.push_back({ playerKey, message, error });
}

static size_t SickoUtf8Length(const std::string& value) {
	size_t count = 0;
	for (unsigned char byte : value)
		if ((byte & 0xC0) != 0x80) ++count;
	return count;
}

static bool SickoValidateAiMessage(std::string& answer) {
	const std::string prefix = "(AI): ";
	const std::string suffix = " - Comando: /ai";
	if (answer.rfind(prefix, 0) != 0 ||
		answer.size() < prefix.size() + suffix.size() ||
		answer.compare(answer.size() - suffix.size(), suffix.size(), suffix) != 0)
		return false;
	if (SickoUtf8Length(answer) > 100 || answer.find('\n') != std::string::npos ||
		answer.find('\r') != std::string::npos || SickoContainsEmoji(answer))
		return false;
	const size_t bodyStart = prefix.size();
	const size_t bodyLength = answer.size() - prefix.size() - suffix.size();
	if (bodyLength == 0) return false;
	std::string body = answer.substr(bodyStart, bodyLength);
	if (body.find('<') != std::string::npos || body.find('>') != std::string::npos)
		return false;
	return true;
}

static void StartSickoAi(PlayerControl* actor, const std::string& question) {
	if (!actor || question.empty()) {
		QueueSickoAiOutput("", "Uso: /ai <pregunta>", true);
		return;
	}
	if (question.size() > 1500) {
		QueueSickoAiOutput("", "La pregunta supera el límite permitido.", true);
		return;
	}
	const char* tokenValue = std::getenv("SMAI_BOT_TOKEN");
	const std::string token = tokenValue ? tokenValue : "";
	if (token.empty()) {
		QueueSickoAiOutput("", "Falta SMAI_BOT_TOKEN en las variables de entorno.", true);
		return;
	}
	const std::string friendCode = SickoGetFriendCode(actor);
	const std::string playerId = SickoHmacPlayerId(friendCode, token);
	if (playerId.empty()) {
		QueueSickoAiOutput("", "No se pudo crear el identificador privado del jugador.", true);
		return;
	}
	if (sickoAiRequestsInFlight.load() >= 1) {
		QueueSickoAiOutput("", "SMAI ya está procesando otras preguntas.", true);
		return;
	}

	std::string requestBody = "{\"player_id\":" + SickoJsonString(playerId) +
		",\"question\":" + SickoJsonString(trim(question)) + ",\"history\":[";
	auto historyIt = sickoAiHistory.find(friendCode);
	if (historyIt != sickoAiHistory.end()) {
		bool first = true;
		for (const auto& item : historyIt->second) {
			if (!first) requestBody += ",";
			first = false;
			requestBody += "{\"role\":" + SickoJsonString(item.role) +
				",\"content\":" + SickoJsonString(item.content) + "}";
		}
	}
	requestBody += "],\"player_memory\":{";
	auto playerData = GetPlayerData(actor);
	const std::string level = playerData ? std::to_string(playerData->fields.PlayerLevel) : "unknown";
	requestBody += "\"level\":" + SickoJsonString(level) +
		",\"device\":" + SickoJsonString(SickoGetPlayerDevice(actor)) +
		",\"game\":\"Among Us\"},\"web_search\":true}";

	// Save the current prompt to the short-lived per-player conversation history.
	auto& playerHistory = sickoAiHistory[friendCode];
	playerHistory.push_back({ "user", trim(question) });
	while (playerHistory.size() > 8) playerHistory.pop_front();
	size_t historyBytes = 0;
	for (const auto& item : playerHistory) historyBytes += item.content.size();
	while (!playerHistory.empty() && historyBytes > 6000) {
		historyBytes -= playerHistory.front().content.size();
		playerHistory.pop_front();
	}

	sickoAiRequestsInFlight.fetch_add(1);
	try {
		std::thread([token, requestBody, friendCode]() {
			struct RequestGuard {
				std::atomic<int>& counter;
				~RequestGuard() { counter.fetch_sub(1); }
			} guard{ sickoAiRequestsInFlight };

			HINTERNET session = WinHttpOpen(L"TheSickMenu-SMAI/2.0",
				WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS, 0);
			if (session) WinHttpSetTimeouts(session, 10000, 10000, 15000, 45000);
			if (!session) {
				QueueSickoAiOutput(friendCode, "No se pudo iniciar la conexión con SMAI.", true);
				return;
			}
			HINTERNET connection = WinHttpConnect(session,
				L"smai-backend-88so.onrender.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
			HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", L"/ai",
				nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
				WINHTTP_FLAG_SECURE) : nullptr;

			std::string response;
			bool ok = request != nullptr;
			std::string headers = "Content-Type: application/json\r\nX-Bot-Token: " + token + "\r\n";
			std::wstring wideHeaders(headers.begin(), headers.end());
			if (ok) ok = WinHttpSendRequest(request, wideHeaders.c_str(),
				static_cast<DWORD>(-1L), const_cast<char*>(requestBody.data()),
				static_cast<DWORD>(requestBody.size()),
				static_cast<DWORD>(requestBody.size()), 0) &&
				WinHttpReceiveResponse(request, nullptr);

			DWORD status = 0, statusSize = sizeof(status);
			if (ok) ok = WinHttpQueryHeaders(request,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
				WINHTTP_NO_HEADER_INDEX) != FALSE;

			if (ok && status == 200) {
				DWORD available = 0;
				do {
					available = 0;
					if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
					if (response.size() + available > 65536) { ok = false; break; }
					std::string chunk(available, '\0');
					DWORD read = 0;
					if (!WinHttpReadData(request, chunk.data(), available, &read)) {
						ok = false;
						break;
					}
					chunk.resize(read);
					response += chunk;
				} while (available > 0);
			}
			if (request) WinHttpCloseHandle(request);
			if (connection) WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);

			if (!ok) {
				QueueSickoAiOutput(friendCode, "Error de conexión con SMAI.", true);
				return;
			}
			if (status != 200) {
				if (status == 422)
					QueueSickoAiOutput(friendCode, "SMAI bloqueó la respuesta; no se publicó.", true);
				else if (status == 401)
					QueueSickoAiOutput(friendCode, "Token de SMAI inválido.", true);
				else
					QueueSickoAiOutput(friendCode, std::format("SMAI devolvió HTTP {}.", status), true);
				return;
			}
			std::string answer;
			if (!SickoExtractJsonString(response, "answer", answer) ||
				!SickoValidateAiMessage(answer)) {
				QueueSickoAiOutput(friendCode, "Respuesta de SMAI inválida; no se publicó.", true);
				return;
			}
			QueueSickoAiOutput(friendCode, answer, false);
		}).detach();
	}
	catch (...) {
		sickoAiRequestsInFlight.fetch_sub(1);
		QueueSickoAiOutput(friendCode, "No se pudo iniciar la petición a SMAI.", true);
	}
}

static void UpdateSickoAiMessages() {
	if (sickoAiOutputCooldown > 0.f)
		sickoAiOutputCooldown -= Time_get_deltaTime(NULL);
	if (Game::pLocalPlayer == nullptr || *Game::pLocalPlayer == nullptr ||
		(!IsInLobby() && !IsInGame()) || sickoAiOutputCooldown > 0.f) return;

	SickoAiOutput output;
	{
		std::lock_guard<std::mutex> lock(sickoAiMutex);
		if (sickoAiOutputQueue.empty()) return;
		output = std::move(sickoAiOutputQueue.front());
		sickoAiOutputQueue.pop_front();
	}
	if (output.error) {
		auto hud = Game::HudManager.GetInstance();
		if (hud && hud->fields.Chat)
			ChatController_AddChatWarning(hud->fields.Chat, convert_to_string(output.message), NULL);
		return;
	}

	std::string answer = output.message;
	if (!SickoValidateAiMessage(answer)) return;
	const std::string prefix = "(AI): ";
	const std::string suffix = " - Comando: /ai";
	const std::string assistantText = answer.substr(prefix.size(),
		answer.size() - prefix.size() - suffix.size());
	auto& history = sickoAiHistory[output.playerKey];
	history.push_back({ "assistant", assistantText });
	while (history.size() > 8) history.pop_front();
	size_t total = 0;
	for (const auto& item : history) total += item.content.size();
	while (!history.empty() && total > 6000) {
		total -= history.front().content.size();
		history.pop_front();
	}
	PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(answer), NULL);
	sickoAiOutputCooldown = 3.2f;
}

static void QueueSickoUtilityMessage(const std::string& message) {
	if (message.empty()) return;
	std::lock_guard<std::mutex> lock(sickoUtilityMutex);
	sickoUtilityOutputQueue.push_back(SickoShorten(message, 120));
}

static void UpdateSickoUtilityMessages() {
	if (Game::pLocalPlayer == nullptr || *Game::pLocalPlayer == nullptr ||
		(!IsInLobby() && !IsInGame())) return;
	std::string message;
	{
		std::lock_guard<std::mutex> lock(sickoUtilityMutex);
		if (sickoUtilityOutputQueue.empty()) return;
		message = std::move(sickoUtilityOutputQueue.front());
		sickoUtilityOutputQueue.pop_front();
	}
	PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(message), NULL);
}

static void StartSickoTranslation(const std::string& language, const std::string& sourceText) {
	// MyMemory requires two concrete ISO language codes; it does not accept
	// "autodetect" as the source language. The command's source language is Spanish.
	const std::string targetLanguage = strToLower(language);
	const std::string requestPath = "/get?q=" + SickoUrlEncode(sourceText) +
		"&langpair=es%7C" + SickoUrlEncode(targetLanguage);
	std::thread([requestPath]() {
		HINTERNET session = WinHttpOpen(L"TheSickMenu/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (session) WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
		if (!session) { QueueSickoUtilityMessage("Translation failed: HTTP session error."); return; }
		HINTERNET connection = WinHttpConnect(session, L"api.mymemory.translated.net",
			INTERNET_DEFAULT_HTTPS_PORT, 0);
		HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET",
			std::wstring(requestPath.begin(), requestPath.end()).c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
		std::string response;
		bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
		if (ok) {
			DWORD status = 0, statusSize = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
			ok = status >= 200 && status < 300;
		}
		if (ok) {
			DWORD available = 0;
			do {
				available = 0;
				if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
				std::string chunk(available, '\0');
				DWORD read = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &read)) { ok = false; break; }
				chunk.resize(read);
				response += chunk;
				if (response.size() > 1024 * 1024) { ok = false; break; }
			} while (available > 0);
		}
		if (request) WinHttpCloseHandle(request);
		if (connection) WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		std::string translated;
		if (ok && SickoExtractJsonString(response, "translatedText", translated) && !translated.empty())
			QueueSickoUtilityMessage(translated);
		else
			QueueSickoUtilityMessage("Translation failed: no valid response from the API.");
	}).detach();
}

static bool HandleSickoUtilityCommand(PlayerControl* actor, const std::string& command, const std::string& rawArgs) {
	if (command == "/tl") {
		if (strToLower(rawArgs).rfind("help", 0) == 0) {
			std::istringstream helpInput(rawArgs);
			std::string helpWord, pageText;
			helpInput >> helpWord;
			int page = 0;
			if (helpInput >> pageText) {
				try {
					size_t parsed = 0;
					page = std::stoi(pageText, &parsed);
					if (parsed != pageText.size() || page < 0) throw std::invalid_argument("page");
				}
				catch (...) {
					QueueSickoUtilityMessage("Usage: /tl help <page_num>");
					return true;
				}
			}
			std::string extra;
			if (helpInput >> extra) {
				QueueSickoUtilityMessage("Usage: /tl help <page_num>");
				return true;
			}
			if (page == 0) {
				QueueSickoUtilityMessage("/tl <language|rnd> \"text\" | /tl help <page_num> | Source: ES (Spanish)");
				return true;
			}
			static const std::vector<std::vector<std::string>> languagePages = {
				{"ES: Español", "EN: English", "FR: Français", "DE: Deutsch", "IT: Italiano", "PT: Português"},
				{"JA: Japanese", "KO: Korean", "ZH: Chinese", "RU: Russian", "NL: Dutch", "PL: Polish"},
				{"AR: Arabic", "HI: Hindi", "TR: Turkish", "UK: Ukrainian", "SV: Swedish", "NO: Norwegian"},
				{"DA: Danish", "FI: Finnish", "EL: Greek", "HE: Hebrew", "ID: Indonesian", "MS: Malay"},
				{"TH: Thai", "VI: Vietnamese", "CS: Czech", "HU: Hungarian", "RO: Romanian", "BG: Bulgarian"}
			};
			if (static_cast<size_t>(page) > languagePages.size()) {
				QueueSickoUtilityMessage(std::format("No language codes on page {}. Pages: 1-{}.", page, languagePages.size()));
				return true;
			}
			std::string listing = std::format("TL codes {}/{}: ", page, languagePages.size());
			for (size_t i = 0; i < languagePages[page - 1].size(); ++i) {
				if (i) listing += " | ";
				listing += languagePages[page - 1][i];
			}
			QueueSickoUtilityMessage(listing);
			return true;
		}		size_t languageEnd = rawArgs.find_first_of(" \t");
		if (languageEnd == std::string::npos) {
			QueueSickoUtilityMessage("Usage: /tl <language> \"text to translate\"");
			return true;
		}
		std::string language = trim(rawArgs.substr(0, languageEnd));
		std::string quoted = trim(rawArgs.substr(languageEnd + 1));
		if (language.empty() || quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"') {
			QueueSickoUtilityMessage("Usage: /tl <language|rnd> \"text to translate\"");
			return true;
		}
		std::string sourceText = quoted.substr(1, quoted.size() - 2);
		if (sourceText.empty() || sourceText.find('"') != std::string::npos || SickoContainsEmoji(sourceText)) {
			QueueSickoUtilityMessage("Invalid text: use one quoted string without emojis.");
			return true;
		}
		if (strToLower(language) == "rnd") {
			static const std::vector<std::string> randomLanguages = {
				"ES", "EN", "FR", "DE", "IT", "PT",
				"JA", "KO", "ZH", "RU", "NL", "PL",
				"AR", "HI", "TR", "UK", "SV", "NO",
				"DA", "FI", "EL", "HE", "ID", "MS",
				"TH", "VI", "CS", "HU", "RO", "BG"
			};
			static std::mt19937 languageRng(static_cast<uint32_t>(
				std::chrono::steady_clock::now().time_since_epoch().count()));
			std::uniform_int_distribution<size_t> pickLanguage(0, randomLanguages.size() - 1);
			language = randomLanguages[pickLanguage(languageRng)];
		}
		StartSickoTranslation(language, sourceText);
		return true;
	}

	if (command == "/rnd") {
		static const std::regex pattern(R"(^\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s+(true|false)\s*$)", std::regex::icase);
		std::smatch match;
		if (!std::regex_match(rawArgs, match, pattern)) {
			QueueSickoUtilityMessage("Usage: /rnd <X>, <Y> <true|false>");
			return true;
		}
		try {
			double x = std::stod(match[1].str()), y = std::stod(match[2].str());
			if (!std::isfinite(x) || !std::isfinite(y) || x > y ||
				std::abs(x) > 1000000000.0 || std::abs(y) > 1000000000.0) {
				QueueSickoUtilityMessage("Invalid range: X must be <= Y and both values must be finite.");
				return true;
			}
			static std::mt19937_64 rng(std::random_device{}());
			if (strToLower(match[3].str()) == "true") {
				std::uniform_real_distribution<double> dist(x, y);
				QueueSickoUtilityMessage(std::format("Random Float {} to {}: {:.6f}", match[1].str(), match[2].str(), dist(rng)));
			}
			else {
				if (std::floor(x) != x || std::floor(y) != y ||
					x < static_cast<double>((std::numeric_limits<int>::min)()) ||
					y > static_cast<double>((std::numeric_limits<int>::max)())) {
					QueueSickoUtilityMessage("Integer mode requires whole-number bounds.");
					return true;
				}
				std::uniform_int_distribution<int> dist(static_cast<int>(x), static_cast<int>(y));
				QueueSickoUtilityMessage(std::format("Random Int {} to {}: {}", match[1].str(), match[2].str(), dist(rng)));
			}
		}
		catch (...) { QueueSickoUtilityMessage("Invalid random range."); }
		return true;
	}

	if (command == "/selfinf") {
		const std::string mode = strToLower(trim(rawArgs));
		if (!mode.empty() && mode != "true" && mode != "false") {
			QueueSickoUtilityMessage("Usage: /selfinf <true|false>");
			return true;
		}
		if (actor == nullptr) {
			QueueSickoUtilityMessage("Player information is unavailable.");
			return true;
		}
		// /selfinf always reports the player who invoked the command, not the
		// client that happens to process the incoming chat message.
		auto requestingPlayer = actor;
		auto data = GetPlayerData(requestingPlayer);
		auto outfit = data ? GetPlayerOutfit(data) : nullptr;
		auto client = (InnerNetClient*)(Game::pAmongUsClient != nullptr ? *Game::pAmongUsClient : nullptr);
		auto clientData = client ? app::InnerNetClient_GetClientFromCharacter(client, requestingPlayer, nullptr) : nullptr;
		static const char* colors[] = { "red", "blue", "green", "pink", "orange", "yellow", "black", "white",
			"purple", "brown", "cyan", "lime", "maroon", "rose", "banana", "gray", "tan", "coral" };
		std::string color = "unknown";
		if (outfit && outfit->fields.ColorId >= 0 && outfit->fields.ColorId < static_cast<int>(sizeof(colors) / sizeof(colors[0])))
			color = colors[outfit->fields.ColorId];
		std::string device = "Unknown";
		if (clientData && clientData->fields.PlatformData) {
			switch (clientData->fields.PlatformData->fields.Platform) {
			case Platforms__Enum::StandaloneEpicPC: device = "Epic Games (PC)"; break;
			case Platforms__Enum::StandaloneSteamPC: device = "Steam (PC)"; break;
			case Platforms__Enum::StandaloneMac: device = "Mac"; break;
			case Platforms__Enum::StandaloneWin10: device = "Microsoft Store (PC)"; break;
			case Platforms__Enum::StandaloneItch: device = "itch.io (PC)"; break;
			case Platforms__Enum::IPhone: device = "iOS"; break;
			case Platforms__Enum::Android: device = "Android"; break;
			case Platforms__Enum::Switch: device = "Nintendo Switch"; break;
			case Platforms__Enum::Xbox: device = "Xbox"; break;
			case Platforms__Enum::Playstation: device = "PlayStation"; break;
			default: break;
			}
		}
		const std::string level = data ? std::to_string(data->fields.PlayerLevel) : "unknown";
		// The game's FriendCode field is read only for the player invoking /selfinf.
		// If the game/account does not expose it, omit it rather than guessing.
		std::string friendCode = (data && data->fields.FriendCode) ?
			convert_from_string(data->fields.FriendCode) : "";
		if (mode == "true") {
			std::string json = "{\"player\":{\"device\":" + SickoJsonString(device) +
				",\"level\":" + SickoJsonString(level) +
				",\"friendcode\":" + SickoJsonString(friendCode.empty() ? "hidden" : friendCode) +
				",\"color\":" + SickoJsonString(color) + "}}";
			QueueSickoUtilityMessage(json);
		}
		else {
			QueueSickoUtilityMessage("Player's info: Device: " + device + ", Lvl: " + level +
				", FC: " + (friendCode.empty() ? "hidden" : friendCode) + ", ColorEng: " + color);
		}
		return true;
	}
	return false;
}

static std::string GenerateSicko8BallAnswer() {
	static std::mt19937 rng(static_cast<uint32_t>(
		std::chrono::steady_clock::now().time_since_epoch().count()));
	static const std::vector<std::string> answers = {
		"Claro que SI",
		"Claro que No... :)",
		"Definitivamente",
		"Estoy ocupado, pregunta más rato w",
		"¿Porque haces esa pregunta?",
		"¿Es cierto?",
		"Busque en internet, y si...",
		"No lo se",
		"Más rato respondo, no tengo tiempo",
		"Es que alguien como tú hace esas preguntas..."
	};
	std::uniform_int_distribution<size_t> distribution(0, answers.size() - 1);
	return answers[distribution(rng)];
}

static bool HandleSickoSocialCommand(PlayerControl* actor, const std::string& message) {
	if (!IsSickoSocialCommand(message)) return false;

	const size_t spacePos = message.find_first_of(" \t");
	const std::string command = strToLower(message.substr(0, spacePos));
	const std::string rawArgs = spacePos == std::string::npos ? "" : trim(message.substr(spacePos + 1));
	if (command == "/tl" || command == "/rnd" || command == "/selfinf")
		return HandleSickoUtilityCommand(actor, command, rawArgs);
	if (command == "/ai") {
		if (rawArgs.empty()) QueueSickoAiOutput("", "Uso: /ai <pregunta>", true);
		else StartSickoAi(actor, rawArgs);
		return true;
	}

	const bool actorIsHost = IsHost() && actor != nullptr &&
		Game::pLocalPlayer != nullptr && actor == *Game::pLocalPlayer;

	if (command == "/startpoll") {
		if (!actorIsHost) {
			QueueSickoPollMessage("Solo el anfitrión puede crear encuestas.");			return true;
		}

		std::vector<std::string> quoted;
		std::string trailing;
		if (!ParseSickoQuotedArguments(rawArgs, quoted, trailing) ||
			quoted.size() < 3 || quoted.size() > 5) {
			QueueSickoPollMessage("Uso: /startpoll \"Pregunta\", \"Opción A\", \"Opción B\" [15-300]");
			return true;
		}

		int duration = 60;
		if (!trailing.empty() && !ParseSickoDuration(trailing, duration)) {
			QueueSickoPollMessage("Duración inválida. Usa entre 15 y 300 segundos.");
			return true;
		}
		if (duration < 15 || duration > 300) {
			QueueSickoPollMessage("Duración inválida. Usa entre 15 y 300 segundos.");
			return true;
		}

		SickoPoll poll;
		poll.question = SickoSanitizePollText(quoted.front());
		poll.durationSeconds = duration;
		auto creatorData = GetPlayerData(actor);
		poll.creatorName = creatorData == nullptr ? "Host" :
			SickoSanitizePollText(convert_from_string(NetworkedPlayerInfo_get_PlayerName(creatorData, nullptr)));
		for (size_t i = 1; i < quoted.size(); ++i)
			poll.options.push_back(SickoSanitizePollText(quoted[i]));

		bool invalidOption = poll.question.empty();
		for (const auto& option : poll.options) invalidOption = invalidOption || option.empty();
		if (invalidOption) {
			QueueSickoPollMessage("La pregunta y todas las opciones deben contener texto.");
			return true;
		}

		if (!sickoActivePoll.has_value() && !sickoPollQueue.empty())
			StartNextSickoPoll();

		if (sickoActivePoll.has_value()) {
			if (sickoPollQueue.size() >= 3) {
				QueueSickoPollMessage("Cola llena: máximo 3 encuestas pendientes.");
				return true;
			}
			sickoPollQueue.push_back(std::move(poll));
			QueueSickoPollMessage(std::format("Encuesta añadida a la cola (#{}).",
				sickoPollQueue.size()));
		}
		else {
			sickoActivePoll = std::move(poll);
			sickoActivePoll->endsAt = std::chrono::steady_clock::now() +
				std::chrono::seconds(sickoActivePoll->durationSeconds);
			QueueSickoPollMessage(std::format("📊 Nueva encuesta: {} ({} s)",
				SickoShorten(sickoActivePoll->question, 78), sickoActivePoll->durationSeconds));
		}
		return true;
	}

	if (command == "/poll") {
		if (!sickoActivePoll.has_value()) {
			QueueSickoPollMessage("No hay ninguna encuesta activa.");
			return true;
		}
		if (actor == nullptr) return true;

		const std::string choice = strToLower(trim(rawArgs));
		if (choice.size() != 1) {
			QueueSickoPollMessage("Uso: /poll A o /poll 1");
			return true;
		}
		int optionIndex = -1;
		const char token = choice[0];
		if (token >= 'a' && token <= 'd') optionIndex = token - 'a';
		else if (token >= '1' && token <= '4') optionIndex = token - '1';
		if (optionIndex < 0 || optionIndex >= static_cast<int>(sickoActivePoll->options.size())) {
			QueueSickoPollMessage("Uso: /poll A o /poll 1 (elige una opción válida).");
			return true;
		}

		const uint32_t playerKey = actor->fields._.NetId;
		auto previousVote = sickoActivePoll->votes.find(playerKey);
		if (previousVote != sickoActivePoll->votes.end() &&
			previousVote->second == static_cast<size_t>(optionIndex)) {
			return true; // Repeating the same vote produces no response.
		}

		const auto now = std::chrono::steady_clock::now();
		auto lastVote = sickoLastPollVote.find(playerKey);
		if (lastVote != sickoLastPollVote.end() &&
			now - lastVote->second < std::chrono::seconds(5)) {
			QueueSickoPollMessage("Espera 5 segundos entre votos.");
			return true;
		}

		sickoActivePoll->votes[playerKey] = static_cast<size_t>(optionIndex);
		sickoLastPollVote[playerKey] = now;
		QueueSickoLiveResults();
		return true;
	}

	if (command == "/pollqueue") {
		if (sickoPollQueue.empty()) {
			QueueSickoPollMessage("No hay encuestas pendientes en la cola.");
			return true;
		}
		std::vector<std::string> queueLines;
		for (size_t i = 0; i < sickoPollQueue.size(); ++i) {
			const auto& poll = sickoPollQueue[i];
			std::string line = std::format("#{} {} ({})",
				i + 1, SickoShorten(poll.question, 22), SickoShorten(poll.creatorName, 10));
			for (size_t j = 0; j < poll.options.size(); ++j) {
				line += std::format(" {}:{}", static_cast<char>('A' + j),
					SickoShorten(poll.options[j], 7));
			}
			queueLines.push_back(line);
		}
		QueueSickoPollMessages(queueLines, false);
		return true;
	}

	if (command == "/cancelpoll") {
		if (actorIsHost && sickoActivePoll.has_value())
			FinishSickoPoll(true);
		return true;
	}

	if (command == "/clearpolls") {
		if (actorIsHost) {
			const size_t removed = sickoPollQueue.size();
			sickoPollQueue.clear();
			QueueSickoPollMessage(std::format("Cola eliminada: {} encuesta(s). La activa continúa.", removed));
		}
		return true;
	}

	if (command == "/8ball") {
		if (actor == nullptr) return true;
		auto playerData = GetPlayerData(actor);
		const std::string playerName = playerData == nullptr ? "Jugador" :
			SickoSanitizePollText(convert_from_string(NetworkedPlayerInfo_get_PlayerName(playerData, nullptr)));
		const std::string question = rawArgs.empty() ? "(sin pregunta)" : SickoSanitizePollText(rawArgs);
		const std::string answer = GenerateSicko8BallAnswer();
		const std::string publicMessage = std::format("- {} pregunta: {}\nLa bola magica dice: {}",
			SickoShorten(playerName, 18), SickoShorten(question, 42), answer);
		if (Game::pLocalPlayer != nullptr && *Game::pLocalPlayer != nullptr)
			PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(publicMessage), NULL);
		return true;
	}

	return true;
}

float calculateChatNewlineSize(std::string playerName) {
	float result = 0.f;
	float maxSize = 100.f;  // track per-line maximum
	size_t i = 0;

	while (i < playerName.size()) {
		// Handle <br>		if (i + 3 < playerName.size() && playerName.compare(i, 4, "<br>") == 0) {
			result += maxSize;
			maxSize = 100.f;
			i += 4;
			continue;
		}
		// Handle newline
		if (playerName[i] == '\n') {
			result += maxSize;
			maxSize = 100.f;
			i++;
			continue;
		}

		// Handle <size=...>
		if (i + 6 < playerName.size() && playerName.compare(i, 6, "<size=") == 0) {
			i += 6;
			size_t endPos = playerName.find('>', i);
			if (endPos == std::string::npos) break;
			std::string valueStr = playerName.substr(i, endPos - i);
			bool isPercent = false;

			if (!valueStr.empty() && valueStr.back() == '%') {
				isPercent = true;
				valueStr.pop_back();
			}

			try {
				float value = std::stof(valueStr);
				if (isPercent) {
					maxSize = (std::max)(maxSize, value);
				}
				else {
					maxSize = (std::max)(maxSize, value * 40.f);
				}
			}
			catch (...) {
				// ignore invalid values
			}

			i = endPos + 1;
			continue;
		}

		// Handle <voffset=...>
		if (i + 9 < playerName.size() && playerName.compare(i, 9, "<voffset=") == 0) {
			i += 9;
			size_t endPos = playerName.find('>', i);
			if (endPos == std::string::npos) break;

			std::string valueStr = playerName.substr(i, endPos - i);

			try {
				float value = std::stof(valueStr);
				if (value < 0) {
					result += (-value) * 40.f;
				}
			}
			catch (...) {
				// ignore invalid values
			}

			i = endPos + 1;
			continue;
		}

		i++; // normal character
	}

	// Add last line's size
	result += maxSize;

	// Subtract 100%
	result -= 100.f;

	return result;
}

void SendPrivateWarnMessage(PlayerControl* toPlayer, const std::string& reason, int totalWarns) {
	if (!State.NotifyWarned || !toPlayer) return;

	if (!*Game::pLocalPlayer) return;

	if (IsHost() && State.ChatCooldown >= 3.f) /* <- In order not to look ridiculous where we are not host :sob: */ {
		std::string message = std::format("You were warned by Reason: {}\n\nTotal warns: {}", reason, totalWarns);
		if (message.length() > 120) {
			message = message.substr(0, 120);
		}

		MessageWriter* writer = InnerNetClient_StartRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient), (*Game::pLocalPlayer)->fields._.NetId, uint8_t(RpcCalls__Enum::SendChat), SendOption__Enum::Reliable, toPlayer->fields._.OwnerId, nullptr);
		MessageWriter_WriteString(writer, convert_to_string(message), nullptr);
		InnerNetClient_FinishRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient), writer, nullptr);
	}
}

void updateCharCounterText(FreeChatInputField* freeChatField) {

	int charLimit = freeChatField->fields.textArea->fields.characterLimit;
	int length = freeChatField->fields.textArea->fields.text->fields.m_stringLength;
	std::string chatCooldownText = "";

	bool commandsAllowed = State.ReadAndSendSickoChat || State.ExtraCommands;

	if (!State.PanicMode && State.ShowChatTimer && !(State.CurrentChatMode == QuickChatModes__Enum::QuickChatOnly && commandsAllowed))
		chatCooldownText = State.ChatCooldown >= 3.f ? (length == 0 ? "" : "<#0f0>You can chat now!</color> ") : std::format("<#f00>Wait for {:.1f} seconds!</color> ", 3 - State.ChatCooldown);
	if (!State.PanicMode && copyNotificationTimer > 0.f) chatCooldownText = std::format("<#0f0>{} text to clipboard!</color> ", isTextCut ? "Cut" : "Copied", copyNotificationTimer);
	std::string charCountColor = std::format("<#{}{}0>", length >= 0.75 * charLimit ? "f" : "0", length < charLimit && length >= 0.75 * charLimit ? "f" : "0");

	if (length < 0.75 * charLimit) {
		Color32 chatTextCol = Color32_op_Implicit(TMP_Text_get_color((TMP_Text*)freeChatField->fields.textArea->fields.outputText, NULL), NULL);

		charCountColor = std::format("<#{:02x}{:02x}{:02x}>",
			chatTextCol.r, chatTextCol.g, chatTextCol.b);
	}

	std::string quickChatInfo = !State.PanicMode && State.CurrentChatMode == QuickChatModes__Enum::QuickChatOnly && commandsAllowed ? "<#f00>(Quick Chat Mode, only commands are allowed)</color> " : "";

	std::string updatedText = std::format("{}{}{}{}/{}</color>",
		quickChatInfo, chatCooldownText, charCountColor, length, charLimit);
	TMP_Text_set_text((TMP_Text*)freeChatField->fields.charCountText, convert_to_string(updatedText), NULL);
}

void ChangeChatNotificationBackground(ChatNotification* chatNotif, PlayerControl* sender) {
	if ((!State.PanicMode || State.TempPanicMode) && chatNotif != NULL) {
		auto textArea = chatNotif->fields.chatText;
		auto bgArea = chatNotif->fields.background;
		if (State.CustomGameTheme && sender != NULL) {
			auto bg32 = Color32();
			bg32.r = int(State.GameBgColor.x * 255); bg32.g = int(State.GameBgColor.y * 255); bg32.b = int(State.GameBgColor.z * 255); bg32.a = 255;
			auto bg = Color32_op_Implicit_1(bg32, NULL);
			auto text32 = Color32();
			text32.r = int(State.GameTextColor.x * 255); text32.g = int(State.GameTextColor.y * 255); text32.b = int(State.GameTextColor.z * 255); text32.a = 255;
			auto textCol = Color32_op_Implicit_1(text32, NULL);
			if (GetPlayerData(sender)->fields.IsDead) bg.a *= 0.75f;
			SpriteRenderer_set_color(bgArea, bg, NULL);
			TMP_Text_set_color((app::TMP_Text*)textArea, textCol, NULL);
			/*auto chatText = convert_from_string(TMP_Text_get_text((app::TMP_Text*)textArea, NULL));
			chatText = std::format("<#{:02x}{:02x}{:02x}>", text32.r, text32.g, text32.b) + chatText + "</color>";
			TMP_Text_set_text((app::TMP_Text*)textArea, convert_to_string(chatText), NULL);*/
		}
		else if (State.DarkMode && sender != NULL) {
			auto black = Color(0.133f, 0.133f, 0.133f, 1.f);
			SpriteRenderer_set_color(bgArea, black, NULL);
			TMP_Text_set_color((app::TMP_Text*)textArea, Palette__TypeInfo->static_fields->White, NULL);
			/*auto chatText = convert_from_string(TMP_Text_get_text((app::TMP_Text*)textArea, NULL));
			chatText = "<#fff>" + chatText + "</color>";
			TMP_Text_set_text((app::TMP_Text*)textArea, convert_to_string(chatText), NULL);*/
		}

		if (State.IsProcessingSickoChat) {
			SpriteRenderer_set_color(bgArea, Color(0.6f, 0.4f, 0.f, 1.f), NULL);
			TMP_Text_set_color((app::TMP_Text*)textArea, Palette__TypeInfo->static_fields->White, NULL);
		}
	}
}
std::string UncensorLink(std::string text, std::string dotReplacer = ",");

void ChangeOtherChatObjectColors(ChatController* chatController, Color col) {
	// https://github.com/the-real-techiee/DarkModeAU/blob/main/DarkMode/DarkModeMain.cs
	auto openBanMenuIcon = GameObject_Find(convert_to_string("OpenBanMenuIcon"), NULL);
	auto openKeyboardIcon = GameObject_Find(convert_to_string("OpenKeyboardIcon"), NULL);
	auto quickChatIcon = GameObject_Find(convert_to_string("QuickChatIcon"), NULL);

	SpriteRenderer* openBanMenuSpriteRenderer = NULL;
	SpriteRenderer* openKeyboardSpriteRenderer = NULL;
	SpriteRenderer* quickChatSpriteRenderer = NULL;

	static std::string spriteRendererTypeName = translate_type_name("UnityEngine.SpriteRenderer, UnityEngine.CoreModule");
	Type* spriteRendererType = app::Type_GetType(convert_to_string(spriteRendererTypeName), NULL);

	if (openBanMenuIcon != NULL) {
		auto openBanMenuComponent = (Component_1*)GameObject_get_transform(openBanMenuIcon, NULL);
		openBanMenuSpriteRenderer = (SpriteRenderer*)Component_GetComponent(openBanMenuComponent, spriteRendererType, NULL);
	}

	if (openKeyboardIcon != NULL) {
		auto openKeyboardComponent = (Component_1*)GameObject_get_transform(openKeyboardIcon, NULL);
		openKeyboardSpriteRenderer = (SpriteRenderer*)Component_GetComponent(openKeyboardComponent, spriteRendererType, NULL);
	}

	if (quickChatIcon != NULL) {
		auto quickChatComponent = (Component_1*)GameObject_get_transform(quickChatIcon, NULL);
		quickChatSpriteRenderer = (SpriteRenderer*)Component_GetComponent(quickChatComponent, spriteRendererType, NULL);
	}

	float brighteningFactor = 2.36f;
	if (col.r * brighteningFactor <= 1 && col.g * brighteningFactor <= 1 && col.b * brighteningFactor <= 1) {
		col.r *= brighteningFactor;
		col.g *= brighteningFactor;
		col.b *= brighteningFactor;
	}

	if (chatController != NULL) {
		// https://github.com/xChipseq/VanillaEnhancements/blob/main/VanillaEnhancements/Patches/DarkModePatches.cs

		auto chatButton = chatController->fields.chatButton;
		auto chatButtonTransform = chatButton == NULL ? NULL : Component_get_transform((Component_1*)chatButton, NULL);

		auto backgroundChild = chatButtonTransform == NULL ? NULL : (Component_1*)Transform_FindChild(chatButtonTransform, convert_to_string("Background"), NULL);
		auto backgroundSprite = backgroundChild == NULL ? NULL : (SpriteRenderer*)Component_GetComponent(backgroundChild, spriteRendererType, NULL);

		auto inactiveChild = chatButtonTransform == NULL ? NULL : (Component_1*)Transform_FindChild(chatButtonTransform, convert_to_string("Inactive"), NULL);
		auto inactiveSprite = inactiveChild == NULL ? NULL : (SpriteRenderer*)Component_GetComponent(inactiveChild, spriteRendererType, NULL);

		auto activeChild = chatButtonTransform == NULL ? NULL : (Component_1*)Transform_FindChild(chatButtonTransform, convert_to_string("Active"), NULL);
		auto activeSprite = activeChild == NULL ? NULL : (SpriteRenderer*)Component_GetComponent(activeChild, spriteRendererType, NULL);

		auto selectedChild = chatButtonTransform == NULL ? NULL : (Component_1*)Transform_FindChild(chatButtonTransform, convert_to_string("Selected"), NULL);
		auto selectedSprite = selectedChild == NULL ? NULL : (SpriteRenderer*)Component_GetComponent(selectedChild, spriteRendererType, NULL);

		if (backgroundSprite != NULL)
			SpriteRenderer_set_color(backgroundSprite, col, NULL);

		if (inactiveSprite != NULL)
			SpriteRenderer_set_color(inactiveSprite, col, NULL);

		/*if (activeSprite != NULL)
			SpriteRenderer_set_color(activeSprite, col, NULL);

		if (selectedSprite != NULL)
			SpriteRenderer_set_color(selectedSprite, col, NULL);*/
	}

	if (openBanMenuSpriteRenderer != NULL)
		SpriteRenderer_set_color(openBanMenuSpriteRenderer, col, NULL);

	if (openKeyboardSpriteRenderer != NULL)
		SpriteRenderer_set_color(openKeyboardSpriteRenderer, col, NULL);

	if (quickChatSpriteRenderer != NULL)
		SpriteRenderer_set_color(quickChatSpriteRenderer, col, NULL);
}

void ShowChatNotification(ChatNotification* chatNotification, PlayerControl* sender, String* message) {
	// this function replicates the actual function used by the game to display chat notifications
	// this is necessary as ChatNotification_SetUp checks if the ShipStatus is null
	// thankfully, ChatNotification_Update takes care of checking if the PlayerCustomizationMenu is active

	chatNotification->fields.timeOnScreen = 5.f; // default in game duration
	auto gameObj = Component_get_gameObject((Component_1*)chatNotification, NULL);
	GameObject_SetActive(gameObj, true, NULL);

	auto pData = GetPlayerData(sender);
	Color32 playerOutlineColor = GetPlayerTextColor(GetPlayerOutfit(pData)->fields.ColorId, true);
	Color32 playerTextColor = GetPlayerTextColor(GetPlayerOutfit(pData)->fields.ColorId);
	std::string colorCode = std::format("<#{:02x}{:02x}{:02x}{:02x}>",
		playerTextColor.r, playerTextColor.g, playerTextColor.b, playerTextColor.a);
	std::string playerName = convert_from_string(GetPlayerOutfit(pData)->fields.PlayerName);
	std::string colorBlindName = convert_from_string(CosmeticsLayer_GetColorBlindText(sender->fields.cosmetics, NULL));
	if (State.IsProcessingSickoChat) colorBlindName += " <b><#fb0>[<#ff006c>SickoChat</color>]</color></b>";

	ChatNotification_SetCosmetics(chatNotification, pData, NULL);
	TMP_Text_set_text((TMP_Text*)chatNotification->fields.playerColorText, convert_to_string(colorBlindName), NULL);
	TMP_Text_set_text((TMP_Text*)chatNotification->fields.playerNameText,
		convert_to_string(colorCode + playerName + "</color>"),
		NULL);
	TMP_Text_set_outlineColor((TMP_Text*)chatNotification->fields.playerNameText, playerOutlineColor, NULL);
	TMP_Text_set_text((TMP_Text*)chatNotification->fields.chatText, message, NULL);

	ChangeChatNotificationBackground(chatNotification, sender);
}

#pragma region CommandHandler

static const std::string SICKO_SOCIALS_MESSAGE = "Check out SickoMenu!\n\nGitHub: github.com/g0aty/SickoMenu\nDisc\u043Erd: Disc\u043Erd.gg/sickos";

// GetCurrentGameModeName / BuildGameRulesMessages are on hold until more gamemodes are added back -
// /preset (below) replaces this system in the meantime, driven by user-saved presets instead of hardcoded rules.
/*
static std::string GetCurrentGameModeName() {
	std::vector<std::string> GAMEMODES = State.DisableHostAnticheat
		? std::vector<std::string>{ "Default", "Task Speedrun", "Battle Royale" }
	: std::vector<std::string>{ "Default", "Task Speedrun" };
	int idx = std::clamp(State.GameMode, 0, (int)GAMEMODES.size() - 1);
	return GAMEMODES[idx];
}

static std::vector<std::string> BuildGameRulesMessages(const std::string& modeName) {
	if (modeName == "Task Speedrun") {
		return {
			"Task Speedrun:\n\nEveryone is a crewmate\nFirst player to complete their tasks wins!\nGame Timer: " +
				std::to_string(State.GameModeDuration) + "s"
		};
	}
	return {};
}
*/

static void SendChatPreset(const Settings::ChatPreset& preset) {
	if (preset.Messages.empty()) return;
	PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(preset.Messages.front()), NULL);
	State.Mod_PendingRulesMessages = {};
	for (size_t i = 1; i < preset.Messages.size(); i++) State.Mod_PendingRulesMessages.push(preset.Messages[i]);
	State.Mod_PendingRulesDelay = 2.0f;
}

static const Settings::ChatPreset* FindChatPresetByName(const std::string& lowerName) {
	for (auto& p : State.ChatPresets) {
		if (strToLower(p.Name) == lowerName) return &p;
	}
	return nullptr;
}

static bool HandleChatCommand(PlayerControl* actor, const std::string& message) {
	if (message.empty() || message[0] != '/') return false;
	if (HandleSickoSocialCommand(actor, message)) return true;
	if (!State.Mod_EnableModeration) return false;
	bool isLocal = (actor == *Game::pLocalPlayer);

	std::string lowerMsg = strToLower(message);
	size_t spacePos = lowerMsg.find(' ');
	std::string cmd = spacePos == std::string::npos ? lowerMsg : lowerMsg.substr(0, spacePos);
	std::string rawArgs = spacePos == std::string::npos ? "" : trim(message.substr(spacePos + 1));
	std::string argsLower = strToLower(rawArgs);

	if (cmd == "/s") cmd = "/start";
	if (cmd == "/w") cmd = "/warn";
	if (cmd == "/uw") cmd = "/unwarn";
	if (cmd == "/cw") cmd = "/checkwarns";

	static const std::set<std::string> KNOWN_COMMANDS = {
	"/color", "/colour", "/preset", "/sicko",
	"/kick", "/kickc", "/ban", "/banc",
	"/warn", "/warnc", "/unwarn", "/unwarnc", "/checkwarns",
	"/callmeeting", "/endmeeting", "/start", "/end",
	};

	if (!KNOWN_COMMANDS.count(cmd) && cmd != "/preset") {
		std::string shorthandName = cmd.substr(1); // strip leading '/'
		const Settings::ChatPreset* shorthandPreset = FindChatPresetByName(shorthandName);
		if (shorthandPreset != nullptr) {
			if (PlayerHasPermission(actor, "preset")) {
				SendChatPreset(*shorthandPreset);
				auto sourceEvt = GetEventPlayerControl(actor);
				if (sourceEvt.has_value()) {
					std::string msg = sourceEvt->playerName + " used preset \"" + shorthandPreset->Name + "\"";
					State.liveConsoleEvents.emplace_back(std::make_unique<ModerationEvent>(sourceEvt.value(), msg));
					
					if (State.ShowConsoleEventsAsToasts &&
						ConsoleGui::IsEventFiltered(EVENT_TYPES::EVENT_MODERATION) &&
						ConsoleGui::IsPlayerFiltered(actor->fields.PlayerId)) {
						Toasts::AddToast("Moderation", msg, ImVec4(1.f, 0.65f, 0.f, 1.f));
					}
				}
			}
			return true;
		}
	}

	if (!KNOWN_COMMANDS.count(cmd)) return false;

	auto localWarn = [&](const std::string& text) {
		if (isLocal) ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string(text), NULL);
		};

	if (cmd == "/color" || cmd == "/colour") {
		if (PlayerHasPermission(actor, "color")) {
			int colorId = FindColorIdByName(argsLower);
			if (colorId != -1) {
				if (IsInGame()) State.rpcQueue.push(new RpcForceColor(actor, (uint8_t)colorId));
				else if (IsInLobby()) State.lobbyRpcQueue.push(new RpcForceColor(actor, (uint8_t)colorId));
			}
		}
	}
	else if (cmd == "/sicko") {
		if (PlayerHasPermission(actor, "sicko")) {
			std::string sickoText = convert_to_string(UncensorLink(SICKO_SOCIALS_MESSAGE, ".­"))->fields.m_stringLength > 120
				? UncensorLink(SICKO_SOCIALS_MESSAGE)
				: UncensorLink(SICKO_SOCIALS_MESSAGE, ".­");
			PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(sickoText), NULL);
		}
	}
	else if (cmd == "/preset") {
		if (PlayerHasPermission(actor, "preset") && !rawArgs.empty()) {
			const Settings::ChatPreset* preset = FindChatPresetByName(argsLower);
			if (preset != nullptr) {
				SendChatPreset(*preset);
				auto sourceEvt = GetEventPlayerControl(actor);
				if (sourceEvt.has_value()) {
					std::string msg = sourceEvt->playerName + " used preset \"" + preset->Name + "\"";
					State.liveConsoleEvents.emplace_back(std::make_unique<ModerationEvent>(sourceEvt.value(), msg));

					if (State.ShowConsoleEventsAsToasts &&
						ConsoleGui::IsEventFiltered(EVENT_TYPES::EVENT_MODERATION) &&
						ConsoleGui::IsPlayerFiltered(actor->fields.PlayerId)) {
						Toasts::AddToast("Moderation", msg, ImVec4(1.f, 0.65f, 0.f, 1.f));
					}
				}
			}
		}
	}
	else if (cmd == "/kick" || cmd == "/kickc" || cmd == "/ban" || cmd == "/banc") {
		if (PlayerHasPermission(actor, (cmd == "/kick" || cmd == "/kickc") ? "kick" : "ban") && !rawArgs.empty()) {
			bool byColor = (cmd == "/kickc" || cmd == "/banc");
			bool isBan = (cmd == "/ban" || cmd == "/banc");
			PlayerControl* target = byColor ? ResolveTargetByColor(FindColorIdByName(argsLower)) : ResolveTargetByName(argsLower);
			if (target != NULL && (IsHost() || GetPlayerMaxRank(actor) > GetPlayerMaxRank(target))) {
				InnerNetClient_KickPlayer((InnerNetClient*)(*Game::pAmongUsClient), target->fields._.OwnerId, isBan, NULL);
				auto sourceEvt = GetEventPlayerControl(actor);
				auto targetEvt = GetEventPlayerControl(target);
				if (sourceEvt.has_value() && targetEvt.has_value()) {
					std::string msg = sourceEvt->playerName + (isBan ? " banned " : " kicked ") + targetEvt->playerName;
					State.liveConsoleEvents.emplace_back(std::make_unique<ModerationEvent>(sourceEvt.value(), msg));

					if (State.ShowConsoleEventsAsToasts &&
						ConsoleGui::IsEventFiltered(EVENT_TYPES::EVENT_MODERATION) &&
						ConsoleGui::IsPlayerFiltered(actor->fields.PlayerId)) {
						Toasts::AddToast("Moderation", msg, ImVec4(1.f, 0.65f, 0.f, 1.f));
					}
				}
			}
			else {
				localWarn("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>Could not find exactly one matching player, or you don't outrank them.</b></font></color>");
			}
		}
	}
	else if (cmd == "/warn" || cmd == "/warnc") {
		if (PlayerHasPermission(actor, "warn") && !rawArgs.empty()) {
			size_t reasonSpace = rawArgs.find(' ');
			if (reasonSpace == std::string::npos) {
				localWarn("<#aaaaaa><size=-0.24><font=\"Barlow-Regular Masked\"><b>Usage: /warn <Name> <Reason> or /warnc <Color> <Reason></b></font></color>");
			}
			else {
				std::string warnTarget = rawArgs.substr(0, reasonSpace);
				std::string warnReason = trim(rawArgs.substr(reasonSpace + 1));
				if (warnReason.empty()) {
					localWarn("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>Warn reason cannot be empty.</b></font></color>");
				}
				else {
					bool byColor = (cmd == "/warnc");
					std::string warnTargetLower = strToLower(warnTarget);
					PlayerControl* target = byColor ? ResolveTargetByColor(FindColorIdByName(warnTargetLower)) : ResolveTargetByName(warnTargetLower);
					if (target == NULL) {
						localWarn(std::format("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>Could not find exactly one matching player for \"{}\".</b></font></color>", warnTarget));
					}
					else {
						auto targetPd = GetPlayerData(target);
						std::string targetName = convert_from_string(NetworkedPlayerInfo_get_PlayerName(targetPd, nullptr));
						std::string targetFc = (targetPd != NULL && targetPd->fields.FriendCode != NULL) ? convert_from_string(targetPd->fields.FriendCode) : "";
						if (targetFc.empty()) {
							localWarn("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>That player has no friend code available.</b></font></color>");
						}
						else {
							State.WarnedFriendCodes[targetFc]++;
							State.WarnReasons[targetFc].push_back(warnReason);
							State.Save();
							if (State.NotifyWarned) SendPrivateWarnMessage(target, warnReason, State.WarnedFriendCodes[targetFc]);
							PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(targetName + " has been warned: " + warnReason), NULL);
							auto sourceEvt = GetEventPlayerControl(actor);
							if (sourceEvt.has_value()) {
								std::string msg = sourceEvt->playerName + " warned " + targetName + ": " + warnReason;
								State.liveConsoleEvents.emplace_back(std::make_unique<ModerationEvent>(sourceEvt.value(), msg));

								if (State.ShowConsoleEventsAsToasts &&
									ConsoleGui::IsEventFiltered(EVENT_TYPES::EVENT_MODERATION) &&
									ConsoleGui::IsPlayerFiltered(actor->fields.PlayerId)) {
									Toasts::AddToast("Moderation", msg, ImVec4(1.f, 0.65f, 0.f, 1.f));
								}
							}
						}
					}
				}
			}
		}
	}
	else if (cmd == "/unwarn" || cmd == "/unwarnc") {
		if (PlayerHasPermission(actor, "warn") && !rawArgs.empty()) {
			size_t numSpace = rawArgs.find(' ');
			if (numSpace == std::string::npos) {
				localWarn("<#aaaaaa><size=-0.24><font=\"Barlow-Regular Masked\"><b>Usage: /unwarn <Name> <WarnNumber> or /unwarnc <Color> <WarnNumber> - use /checkwarns <FriendCode> to see a player's warn numbers</b></font></color>");
			}
			else {
				std::string unwarnTarget = rawArgs.substr(0, numSpace);
				std::string numberStr = trim(rawArgs.substr(numSpace + 1));
				int reasonIndex = -1;
				try { reasonIndex = std::stoi(numberStr) - 1; }
				catch (...) { reasonIndex = -1; }
				if (reasonIndex < 0) {					localWarn("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>Invalid WarnNumber.</b></font></color>");
				}
				else {
					bool byColor = (cmd == "/unwarnc");
					std::string unwarnTargetLower = strToLower(unwarnTarget);
					PlayerControl* target = byColor ? ResolveTargetByColor(FindColorIdByName(unwarnTargetLower)) : ResolveTargetByName(unwarnTargetLower);
					if (target == NULL) {
						localWarn(std::format("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>Could not find exactly one matching player for \"{}\".</b></font></color>", unwarnTarget));
					}
					else {
						auto targetPd = GetPlayerData(target);
						std::string targetName = convert_from_string(NetworkedPlayerInfo_get_PlayerName(targetPd, nullptr));
						std::string fc = (targetPd != NULL && targetPd->fields.FriendCode != NULL) ? convert_from_string(targetPd->fields.FriendCode) : "";
						auto it = State.WarnReasons.find(fc);
						if (fc.empty() || it == State.WarnReasons.end() || reasonIndex >= (int)it->second.size()) {
							localWarn(std::format("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>Invalid WarnNumber, or \"{}\" has no warns.</b></font></color>", targetName));
						}
						else {
							it->second.erase(it->second.begin() + reasonIndex);
							if (--State.WarnedFriendCodes[fc] <= 0) {
								State.WarnedFriendCodes.erase(fc);
								State.WarnReasons.erase(fc);
							}
							State.Save();
							PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string("Removed warn #" + std::to_string(reasonIndex + 1) + " from " + targetName), NULL);
						}
					}
				}
			}
		}
	}
	else if (cmd == "/checkwarns") {
		if (PlayerHasPermission(actor, "warn") && !rawArgs.empty()) {
			auto it = State.WarnReasons.find(rawArgs);
			if (it != State.WarnReasons.end() && !it->second.empty()) {
				if (isLocal) {
					std::string allReasons;
					for (size_t i = 0; i < it->second.size(); ++i) {
						allReasons += std::format("[{}] {}", i + 1, it->second[i]);
						if (i + 1 < it->second.size()) allReasons += "\n";
					}
					localWarn(std::format("<#ffff00><size=-0.24><font=\"Barlow-Regular Masked\"><b>All warns for <#FFF>\"{}\":\n\n{}</b></font></color>", rawArgs, allReasons));
				}
				else {
					std::string list = "Warns for " + rawArgs + ": ";
					for (size_t i = 0; i < it->second.size(); ++i) {
						list += "[" + std::to_string(i + 1) + "] " + it->second[i];
						if (i + 1 < it->second.size()) list += " | ";
					}
					if (list.length() > 120) list = list.substr(0, 117) + "...";
					PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(list), NULL);
				}
			}
			else {
				if (isLocal) localWarn(std::format("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>No warns found for \"{}\".</b></font></color>", rawArgs));
				else PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string("No warns found for " + rawArgs), NULL);
			}
		}
	}
	else if (cmd == "/callmeeting") {
		if (PlayerHasPermission(actor, "callmeeting") && IsInGame() && !State.InMeeting) {
			RepairSabotage(*Game::pLocalPlayer);
			State.rpcQueue.push(new RpcReportBody({}));
		}
	}
	else if (cmd == "/endmeeting") {
		if (PlayerHasPermission(actor, "endmeeting") && State.InMeeting) {
			State.rpcQueue.push(new RpcEndMeeting());
			State.InMeeting = false;
		}
	}
	else if (cmd == "/start") {
		if (PlayerHasPermission(actor, "start") && IsInLobby()) {
			InnerNetClient_SendStartGame((InnerNetClient*)(*Game::pAmongUsClient), NULL);
		}
	}
	else if (cmd == "/end") {
		if (PlayerHasPermission(actor, "end") && IsInGame()) {
			State.rpcQueue.push(new RpcEndGame(GameOverReason__Enum(0)));
		}
	}

	return true;
}

#pragma endregion

void dChatController_AddChat(ChatController* __this, PlayerControl* sourcePlayer, String* chatText, bool censor, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dChatController_AddChat executed", false);
	censor = IsChatCensored(); // Fix chat not being censored
	if (!State.PanicMode) {
		auto player = GetPlayerData(sourcePlayer);
		auto local = GetPlayerData(*Game::pLocalPlayer);
		std::string message = convert_from_string(chatText);
		std::string newChatText = message;
		bool isSickoPollSystemMessage = false;
		if (sourcePlayer != nullptr && !message.empty() && message[0] != '/')
			RecordSickoAiHistory(sourcePlayer, message);
		if (IsHost()) {
			auto sentMessage = std::find(sickoPollSentMessages.begin(), sickoPollSentMessages.end(), message);
			if (sentMessage != sickoPollSentMessages.end()) {
				sickoPollSentMessages.erase(sentMessage);
				isSickoPollSystemMessage = true;
			}
		}
		if (IsHost() && sourcePlayer != *Game::pLocalPlayer) {
			const bool handled = HandleChatCommand(sourcePlayer, message);
			if (handled && IsSickoSocialCommand(message)) return;
		}

		// A non-host client with TheSickMenu can act as the 8-ball responder.
		// Incoming commands are processed once when they arrive; the reply is sent
		// as a normal public chat RPC from this modded client.
		// Utility commands are also accepted from other players. The sender's
		// own mod intercepts local commands; this path lets an unmodded sender
		// use them when at least one TheSickMenu client is present.
		if (sourcePlayer != nullptr && Game::pLocalPlayer != nullptr &&
			*Game::pLocalPlayer != nullptr && sourcePlayer != *Game::pLocalPlayer &&
			IsSickoSocialCommand(message)) {
			const size_t commandEnd = message.find_first_of(" \t");
			const std::string incomingCommand = strToLower(message.substr(0, commandEnd));
			if (incomingCommand == "/tl" || incomingCommand == "/rnd" || incomingCommand == "/selfinf" || incomingCommand == "/ai") {
				HandleSickoSocialCommand(sourcePlayer, message);
				return; // Hide the raw command; the result is sent as public chat.
			}
		}

		if (!IsHost() && sourcePlayer != nullptr && Game::pLocalPlayer != nullptr &&
			*Game::pLocalPlayer != nullptr && sourcePlayer != *Game::pLocalPlayer &&
			IsSicko8BallCommand(message)) {
			HandleSickoSocialCommand(sourcePlayer, message);
			return; // The public reply contains the question; do not display the raw command too.
		}
		if (State.BetterChatNotifications && __this->fields.state == ChatControllerState__Enum::Closed &&
			(State.ReadGhostMessages || local->fields.IsDead || !player->fields.IsDead) &&
			player == ((!State.SafeMode && State.playerToChatAs.has_value()) ?
			State.playerToChatAs.validate().get_PlayerData() : local)) {
			auto chatNotif = Game::HudManager.GetInstance()->fields.Chat->fields.chatNotification;
			ShowChatNotification(chatNotif, sourcePlayer, chatText);
		}
		std::string playerName = convert_from_string(NetworkedPlayerInfo_get_PlayerName(player, nullptr));
		if (State.CustomName && !State.ServerSideCustomName && (sourcePlayer == *Game::pLocalPlayer || State.CustomNameForEveryone)) {
			playerName = GetCustomName(playerName);
		}
		float newlineSize = calculateChatNewlineSize(playerName);
		if (newlineSize > 0.f) {
			newChatText = std::format("<size={}%><#0000>0</color></size>\n", newlineSize) + newChatText;
		}
		if (State.ReadGhostMessages) {
			bool wasDead = false;

			if (player != NULL && player->fields.IsDead && local != NULL && !local->fields.IsDead) {
				local->fields.IsDead = true;
				wasDead = true;
			}
			chatText = convert_to_string(newChatText);
			ChatController_AddChat(__this, sourcePlayer, chatText, censor, method);
			auto outfit = GetPlayerOutfit(player);
			uint32_t colorId = outfit->fields.ColorId;
			if (wasDead) {
				local->fields.IsDead = false;
			}		}
		else {
			chatText = convert_to_string(newChatText);
			ChatController_AddChat(__this, sourcePlayer, chatText, censor, method);
		}
		if (State.Enable_SMAC) {
			static const std::set<std::string> KNOWN_COMMAND_WORDS = {
				"/help", "/h", "/cmds", "/color", "/colour", "/sicko", "/r", "/rules", "/s", "/start", "/end",
				"/kick", "/kickc", "/ban", "/banc",
				"/warn", "/w", "/warnc", "/unwarn", "/uw", "/unwarnc", "/checkwarns", "/cw",
				"/callmeeting", "/endmeeting", "/8ball", "/tl", "/rnd", "/selfinf",
			};
			std::string firstWordLower = strToLower(message.substr(0, message.find(' ')));
			bool isRecognizedCommand = KNOWN_COMMAND_WORDS.count(firstWordLower) > 0;
			if (State.SMAC_CheckChat && !isRecognizedCommand && !isSickoPollSystemMessage &&
				((IsInGame() && !State.InMeeting && !player->fields.IsDead) || chatText->fields.m_stringLength > 120)) {
				SMAC_OnCheatDetected(sourcePlayer, "Abnormal Chat");
			}
			if (State.SMAC_CheckBadWords) {
				std::string lowerMessage = strToLower(message);
				std::string firstWordBad = lowerMessage.substr(0, lowerMessage.find(' '));
				for (auto& wordEntry : State.SMAC_BadWords) {
					std::string lowerWord = strToLower(wordEntry.first);
					if (lowerWord.empty()) continue;
					bool matched = wordEntry.second ? (lowerMessage.find(lowerWord) != std::string::npos) : (firstWordBad == lowerWord);
					if (matched) {
						SMAC_OnCheatDetected(sourcePlayer, "Bad Word: " + wordEntry.first);
						break;
					}
				}
			}

			if (State.SMAC_CheckStartWords && !IsInGame()) {
				std::string lowerMessage = strToLower(message);
				std::string firstWordStart = lowerMessage.substr(0, lowerMessage.find(' '));
				uint8_t chatterId = sourcePlayer->fields.PlayerId;
				for (auto& wordEntry : State.SMAC_StartWords) {
					std::string lowerWord = strToLower(wordEntry.first);
					if (lowerWord.empty()) continue;
					bool matched = wordEntry.second ? (lowerMessage.find(lowerWord) != std::string::npos) : (firstWordStart == lowerWord);
					if (matched) {
						if (++State.SMAC_StartWordsCount[chatterId] >= State.SMAC_StartWordsThreshold) {
							SMAC_OnCheatDetected(sourcePlayer, "Start Word: " + wordEntry.first);
							State.SMAC_StartWordsCount[chatterId] = 0;
						}
						break;
					}
				}
			}
		}
		if (State.BetterMessageSounds && (State.ReadGhostMessages || !player->fields.IsDead) && player != local &&
			(__this->fields.state == ChatControllerState__Enum::Open || __this->fields.state == ChatControllerState__Enum::Opening)) {
			auto audioSource = SoundManager_PlaySound(SoundManager__TypeInfo->static_fields->instance, (AudioClip*)__this->fields.messageSound, false, 1.f, NULL, NULL);
			AudioSource_set_pitch(audioSource, 0.5f + (float)sourcePlayer->fields.PlayerId / 15, NULL);
		}
	}
	else {
		ChatController_AddChat(__this, sourcePlayer, chatText, censor, method);
	}
}

void dChatController_SetVisible(ChatController* __this, bool visible, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dChatController_SetVisible executed", false);
	if (State.ChatAlwaysActive && !State.PanicMode)
		ChatController_SetVisible(__this, true, method);
	else
	{
		State.ChatActiveOriginalState = visible;
		ChatController_SetVisible(__this, visible, method);
	}
}

void dChatBubble_SetName(ChatBubble* __this, String* playerName, bool isDead, bool voted, Color color, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dChatBubble_SetName executed", false);
	if (!State.PanicMode && (IsInGame() || IsInLobby())) {
		for (auto playerData : GetAllPlayerData()) {
			auto outfit = GetPlayerOutfit(playerData);
			if (outfit == NULL) continue;
			if (playerName == NetworkedPlayerInfo_get_PlayerName(playerData, nullptr)) {
				auto localData = GetPlayerData(*Game::pLocalPlayer);
				color = State.RevealRoles ? GetRoleColor(playerData->fields.Role) :
					(PlayerIsImpostor(localData) && PlayerIsImpostor(playerData) ? Palette__TypeInfo->static_fields->ImpostorRed : Palette__TypeInfo->static_fields->White);
				if (State.CustomName && !State.ServerSideCustomName && (playerData == GetPlayerData(*Game::pLocalPlayer) || State.CustomNameForEveryone)) {
					playerName = convert_to_string(GetCustomName(convert_from_string(playerName)));
				}
				if (State.RevealRoles && IsInGame()) {
					playerName = convert_to_string("<size=50%>" + GetRoleName(playerData->fields.Role, State.AbbreviatedRoleNames) + "</size> " + convert_from_string(playerName));
				}

				if (State.PlayerColoredDots) {
					Color32&& nameColor = GetPlayerColor(outfit->fields.ColorId);
					std::string dot = std::format("<#{:02x}{:02x}{:02x}{:02x}>●</color>",
						nameColor.r, nameColor.g, nameColor.b,
						nameColor.a);

					if (playerData != GetPlayerData(*Game::pLocalPlayer))
						playerName = convert_to_string(convert_from_string(playerName) + " " + dot);
					else
						playerName = convert_to_string(dot + " " + convert_from_string(playerName));
				}
				if (State.IsProcessingSickoChat) {
					std::string prefix = "<size=90%><#fb0>[<#ff006c>SickoChat</color>]</color> ";
					playerName = convert_to_string(prefix + convert_from_string(playerName) + "</size>");
				}
			}
		}
	}
	ChatBubble_SetName(__this, playerName, isDead, voted, color, method);
}

void dChatController_Update(ChatController* __this, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dChatController_Update executed", false);
	UpdateSickoPolls();
	UpdateSickoUtilityMessages();
	UpdateSickoAiMessages();
	auto freeChatField = __this->fields.freeChatField;
	int length = freeChatField->fields.textArea->fields.text->fields.m_stringLength;

	auto pool = __this->fields.chatBubblePool;
	if (pool->fields.poolSize == 20 && State.ExtendChatHistory) {
		pool->fields.poolSize = 127; // Weird glitches happen to the chat if we extend it past 127
		ObjectPoolBehavior_ReclaimOldest(pool, NULL);
	}
	if (pool->fields.poolSize == 127 && !State.ExtendChatHistory) {
		pool->fields.poolSize = 20;
		ObjectPoolBehavior_ReclaimOldest(pool, NULL);
	}

	if (__this->fields.state != ChatControllerState__Enum::Closed) {
		if (State.PanicMode) freeChatField->fields.textArea->fields.characterLimit = 100;
		else freeChatField->fields.textArea->fields.characterLimit = State.SafeMode ? (State.ExtendChatLimit ? 120 : 100) : 2147483647;

		freeChatField->fields.textArea->fields.allowAllCharacters = !State.PanicMode;
		freeChatField->fields.textArea->fields.AllowEmail = !State.PanicMode;
		freeChatField->fields.textArea->fields.AllowSymbols = !State.PanicMode;

		freeChatField->fields.charCountText->fields._.m_enableWordWrapping = State.PanicMode;

		updateCharCounterText(freeChatField);
		if (State.CurrentChatMode == QuickChatModes__Enum::QuickChatOnly) ChatController_UpdateChatMode(__this, NULL);
		// Always update quick chat chatting mode
	}

	if (!State.SafeMode || State.CurrentChatMode == QuickChatModes__Enum::QuickChatOnly || State.WasPreviousMessageCommand) {
		__this->fields.timeSinceLastMessage = 420.69f; //we can set this to anything more than or equal to 3 and it'll work
		if (State.WasPreviousMessageCommand) State.WasPreviousMessageCommand = false;
	}

	if ((!State.PanicMode || State.TempPanicMode) && (State.CustomGameTheme || State.DarkMode)) {
		if (State.CustomGameTheme) {
			auto bg32 = Color32();
			bg32.r = int(State.GameBgColor.x * 255); bg32.g = int(State.GameBgColor.y * 255); bg32.b = int(State.GameBgColor.z * 255); bg32.a = 255;
			auto bg = Color32_op_Implicit_1(bg32, NULL);
			auto text32 = Color32();
			text32.r = int(State.GameTextColor.x * 255); text32.g = int(State.GameTextColor.y * 255); text32.b = int(State.GameTextColor.z * 255); text32.a = 255;
			auto textCol = Color32_op_Implicit_1(text32, NULL);
			if (__this->fields.freeChatField != NULL) {
				auto outputText = __this->fields.freeChatField->fields.textArea->fields.outputText;
				TMP_Text_set_color((app::TMP_Text*)outputText, textCol, NULL);
				auto col = SpriteRenderer_get_color(__this->fields.freeChatField->fields._.background, NULL);
				bool isHighlighted = col.r == 0.f && col.g == 1.f && col.b == 0.f && col.a == 1.f;				if (!isHighlighted)
					SpriteRenderer_set_color(__this->fields.freeChatField->fields._.background, bg, NULL);
				else {
					auto green32 = Color32();
					green32.r = 34; green32.g = 100; green32.b = 34; green32.a = 255;
					auto green = Color32_op_Implicit_1(green32, NULL);
					SpriteRenderer_set_color(__this->fields.freeChatField->fields._.background, green, NULL);
				}
			}
			if (__this->fields.quickChatField != NULL) {
				auto text = __this->fields.quickChatField->fields.text;
				auto placeholderText = __this->fields.quickChatField->fields.placeholderText;
				TMP_Text_set_color((app::TMP_Text*)text, textCol, NULL);
				TMP_Text_set_color((app::TMP_Text*)placeholderText, textCol, NULL);
				auto col = SpriteRenderer_get_color(__this->fields.quickChatField->fields._.background, NULL);
				bool isHighlighted = col.r == 0.f && col.g == 1.f && col.b == 0.f && col.a == 1.f;
				if (!isHighlighted)
					SpriteRenderer_set_color(__this->fields.quickChatField->fields._.background, bg, NULL);
				else {
					auto green32 = Color32();
					green32.r = 34; green32.g = 100; green32.b = 34; green32.a = 255;
					auto green = Color32_op_Implicit_1(green32, NULL);
					SpriteRenderer_set_color(__this->fields.quickChatField->fields._.background, green, NULL);
				}
			}

			SpriteRenderer* backgroundSpriteRenderer = Game::HudManager.GetInstance()->fields.Chat->fields.backgroundImage;
			if (backgroundSpriteRenderer != NULL)
				SpriteRenderer_set_color(backgroundSpriteRenderer, bg, NULL);

			if (State.DarkMode) {
				auto gray = Color(0.5f, 0.5f, 0.5f, 1.f);
				ChangeOtherChatObjectColors(__this, gray);
			}
			else {
				ChangeOtherChatObjectColors(__this, Palette__TypeInfo->static_fields->White);
			}
		}
		else if (State.DarkMode) {
			auto gray32 = Color32();
			gray32.r = 34; gray32.g = 34; gray32.b = 34; gray32.a = 255;
			auto gray = Color32_op_Implicit_1(gray32, NULL);
			auto green32 = Color32();
			green32.r = 34; green32.g = 100; green32.b = 34; green32.a = 255;
			auto green = Color32_op_Implicit_1(green32, NULL);
			if (__this->fields.freeChatField != NULL) {
				auto outputText = __this->fields.freeChatField->fields.textArea->fields.outputText;
				TMP_Text_set_color((app::TMP_Text*)outputText, Palette__TypeInfo->static_fields->White, NULL);
				auto col = SpriteRenderer_get_color(__this->fields.quickChatField->fields._.background, NULL);
				bool isHighlighted = col.r == 0.f && col.g == 1.f && col.b == 0.f && col.a == 1.f;
				SpriteRenderer_set_color(__this->fields.freeChatField->fields._.background, isHighlighted ? green : gray, NULL);
			}
			if (__this->fields.quickChatField != NULL) {
				auto text = __this->fields.quickChatField->fields.text;
				auto placeholderText = __this->fields.quickChatField->fields.placeholderText;
				TMP_Text_set_color((app::TMP_Text*)text, Palette__TypeInfo->static_fields->White, NULL);
				TMP_Text_set_color((app::TMP_Text*)placeholderText, Palette__TypeInfo->static_fields->White, NULL);
				auto col = SpriteRenderer_get_color(__this->fields.quickChatField->fields._.background, NULL);
				bool isHighlighted = col.r == 0.f && col.g == 1.f && col.b == 0.f && col.a == 1.f;
				SpriteRenderer_set_color(__this->fields.quickChatField->fields._.background, isHighlighted ? green : gray, NULL);
			}

			SpriteRenderer* backgroundSpriteRenderer = Game::HudManager.GetInstance()->fields.Chat->fields.backgroundImage;
			if (backgroundSpriteRenderer != NULL)
				SpriteRenderer_set_color(backgroundSpriteRenderer, gray, NULL);

			ChangeOtherChatObjectColors(__this, Color(0.5f, 0.5f, 0.5f, 1.f));
		}
	}
	else {
		if (__this->fields.freeChatField != NULL) {
			auto outputText = __this->fields.freeChatField->fields.textArea->fields.outputText;
			TMP_Text_set_color((app::TMP_Text*)outputText, Palette__TypeInfo->static_fields->Black, NULL);
			auto col = SpriteRenderer_get_color(__this->fields.freeChatField->fields._.background, NULL);
			bool isHighlighted = col.r == 0.f && col.g == 1.f && col.b == 0.f && col.a == 1.f;
			if (!isHighlighted)
				SpriteRenderer_set_color(__this->fields.freeChatField->fields._.background, Palette__TypeInfo->static_fields->White, NULL);
		}
		if (__this->fields.quickChatField != NULL) {
			auto text = __this->fields.quickChatField->fields.text;
			TMP_Text_set_color((app::TMP_Text*)text, Palette__TypeInfo->static_fields->Black, NULL);
			auto col = SpriteRenderer_get_color(__this->fields.quickChatField->fields._.background, NULL);
			bool isHighlighted = col.r == 0.f && col.g == 1.f && col.b == 0.f && col.a == 1.f;
			if (!isHighlighted)
				SpriteRenderer_set_color(__this->fields.quickChatField->fields._.background, Palette__TypeInfo->static_fields->White, NULL);
		}

		SpriteRenderer* backgroundSpriteRenderer = Game::HudManager.GetInstance()->fields.Chat->fields.backgroundImage;
		if (backgroundSpriteRenderer != NULL)
			SpriteRenderer_set_color(backgroundSpriteRenderer, Palette__TypeInfo->static_fields->White, NULL);

		ChangeOtherChatObjectColors(__this, Palette__TypeInfo->static_fields->White);
	}

	State.MessageSound = (AudioClip*)__this->fields.messageSound;

	auto chatText = __this->fields.freeChatField->fields.textArea->fields.text;
	__this->fields.freeChatField->fields.textArea->fields.AllowPaste = State.ChatPaste && !State.PanicMode && length == 0;
	// Only allow pasting if the field is empty and chat paste is enabled
	bool isCtrl = ImGui::IsKeyDown(0x11) || ImGui::IsKeyDown(0xA2) || ImGui::IsKeyDown(0xA3);
	bool isCpressed = ImGui::IsKeyPressed(0x43) || ImGui::IsKeyDown(0x63);
	bool isXpressed = ImGui::IsKeyPressed(0x58) || ImGui::IsKeyDown(0x78);
	if (State.ChatPaste && isCtrl && (isCpressed || isXpressed) && convert_from_string(chatText) != "") {
		ClipboardHelper_PutClipboardString(chatText, NULL); //ctrl+c
		copyNotificationTimer = 1.5f; // Show copy notification for 1.5 seconds
		if (isXpressed) {
			FreeChatInputField_Clear(__this->fields.freeChatField, NULL); //ctrl+x
			isTextCut = true; // Set flag to indicate text was cut
		}
		else isTextCut = false; // Reset flag if only copied
	}

	if (copyNotificationTimer > 0.f) {
		copyNotificationTimer -= Time_get_deltaTime(NULL);
		if (copyNotificationTimer <= 0.f) copyNotificationTimer = 0.f;
	}

	if (State.MessageSent && State.SafeMode) {
		__this->fields.timeSinceLastMessage = 0.f;
		State.MessageSent = false;
	}
	State.ChatCooldown = __this->fields.timeSinceLastMessage;
	State.ChatFocused = __this->fields.freeChatField->fields.textArea->fields.hasFocus;

	if (!(IsHost() || !State.SafeMode)) State.ChatSpamMode = 0;

	if (IsChatValid(State.chatMessage)) {
		if (!State.PanicMode && State.SafeMode && State.ChatSpam && (State.ChatSpamMode == 0 || State.ChatSpamMode == 2) && (IsInGame() || IsInLobby()) && __this->fields.timeSinceLastMessage >= 3.5f) {
			PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(State.chatMessage), NULL);
			//remove rpc queue stuff cuz of delay and anticheat kick
			State.MessageSent = true;
		}
		if (!State.PanicMode && State.SafeMode && State.CrashChatSpam && (State.ChatSpamMode == 0 || State.ChatSpamMode == 2) && (IsInGame() || IsInLobby()) && __this->fields.timeSinceLastMessage >= 3.5f) {
			PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(State.chatMessage), NULL);
			//remove rpc queue stuff cuz of delay and anticheat kick
			State.MessageSent = true;
		}
	}

	if (!State.PanicMode && State.AprilFoolsMode && State.BrainrotEveryone && (IsInGame() || IsInLobby()) && (__this->fields.timeSinceLastMessage >= 3.5f || !State.SafeMode)) {
		std::vector<std::string> brainrotList = { "Crazy? I was crazy once. They locked me in a room. A rubber room with Fucksons, and Fucksons give me rats.",
			"I like my cheese drippy bruh", "Imagine if Ninja got a low taper fade", "I woke up in Ohio, feeling kinda fly", "What trollface are you?",
			"Skibidi dop dop dop yes yes", "From the gyatt to the sus to the rizz to the mew", "Yeah I'm edging in Ohio, fanum taxing as I goon",
			"You gotta give him that Hawk TUAH and spit on that thang", "Sticking out your gyatt for the rizzler", "I'm Baby Gronk from Ohio",
			"19 dollar fortnite card, who wants it?", "Erm, what the sigma?", "I'll take a double triple Grimace Shake on a gyatt",
			"I know I'm a SIGMA but that doesnt mean I can't have a GYATT too", "Just put the fries in the bag bro", "Stay on the sigma grindset",
			"Sigma Sigma on the wall, who is the skibidiest of them all?", "Duke Dennis did you pray today?", "What kinda bomboclat dawg are ya" };
		auto player = !State.SafeMode && State.playerToChatAs.has_value() ? State.playerToChatAs.validate().get_PlayerControl() : *Game::pLocalPlayer;
		PlayerControl_RpcSendChat(player, convert_to_string(brainrotList[randi(0, (int)brainrotList.size() - 1)]), NULL);
		State.MessageSent = true;
	}

	if (!State.PanicMode && State.AprilFoolsMode && State.RizzUpEveryone && (IsInGame() || IsInLobby()) && (__this->fields.timeSinceLastMessage >= 3.5f || !State.SafeMode)) {
		std::vector<std::string> rizzLinesList = { "Do you have some Ohio rizz? Because you just turned my brain into pure jelly!",
			"If beauty were a Skibidi Toilet, you'd be the one everyone’s trying to get next to!", "Is your name Ohio? Because you’re making my heart do the Skibidi!",
			"Is your aura made of coffee? Because you’re brewing up some strong feelings in me!", "I see dat gyatt and I wanna fanum tax some of dat",
			"Am I Baby Gronk? Because you can be my Livvy Dunne", "Sup shawty, are you skibidi, because I could use that to my sigma", "Hey shawty, are you skibidi rizz in ohio?",
			"Yer a rizzard Harry", "Remind me what a work of skibidi rizz looks like" };
		auto player = !State.SafeMode && State.playerToChatAs.has_value() ? State.playerToChatAs.validate().get_PlayerControl() : *Game::pLocalPlayer;
		PlayerControl_RpcSendChat(player, convert_to_string(rizzLinesList[randi(0, (int)rizzLinesList.size() - 1)]), NULL);		State.MessageSent = true;
	}

	ChatController_Update(__this, method);

	/*if ((!State.PanicMode || State.TempPanicMode) && (State.DarkMode || State.CustomGameTheme) && __this->fields.freeChatField != NULL) {
		//__this->fields.freeChatField->fields.textArea->fields.compoText = convert_to_string(RemoveHtmlTags(convert_from_string(__this->fields.freeChatField->fields.textArea->fields.compoText)));
	}*/
	//nah fuck compoText
}

bool dTextBoxTMP_IsCharAllowed(TextBoxTMP* __this, uint16_t unicode_char, MethodInfo* method)
{
	if (State.ShowHookLogs) Log.HookDebug("Hook dTextBoxTMP_IsCharAllowed executed", false);
	if (__this->fields.ForceUppercase) return TextBoxTMP_IsCharAllowed(__this, unicode_char, method);
	// Patch lobby codes

	//0x08 is backspace, 0x0D is carriage return, 0x7F is delete character, 0x3C is <, 0x3E is >, 0x5B is [
	//lobby codes force uppercase, and we don't change that to fix joining a lobby with code not working
	if (!State.PanicMode) return (unicode_char != 0x08 && unicode_char != 0x0D && unicode_char != 0x7F && ((State.SafeMode && unicode_char != 0x3C && unicode_char != 0x3E && unicode_char != 0x5B) || !State.SafeMode));
	return TextBoxTMP_IsCharAllowed(__this, unicode_char, method);
}

void dTextBoxTMP_SetText(TextBoxTMP* __this, String* input, String* inputCompo, MethodInfo* method)
{
	if (State.ShowHookLogs) Log.HookDebug("Hook dTextBoxTMP_SetText executed", false);
	if (__this->fields.ForceUppercase) {
		__this->fields.SendOnFullChars = State.PanicMode || !State.BetterLobbyCodeInput;
		__this->fields.ClearOnFocus = State.PanicMode || !State.BetterLobbyCodeInput;
		return TextBoxTMP_SetText(__this, input, inputCompo, method);
		// Patch lobby codes
	}
	else if (!State.PanicMode) {
		if (!State.SafeMode)
			__this->fields.characterLimit = 2147483647;
		else
			__this->fields.characterLimit = State.ExtendChatLimit ? 120 : 100;
	}
	else __this->fields.characterLimit = 100;
	//inputCompo = convert_to_string(RemoveHtmlTags(convert_from_string(inputCompo))); // Fix #fff/color bug in text input field
	//nah fuck compoText

	TextBoxTMP_SetText(__this, input, inputCompo, method);
}

std::string UncensorLink(std::string text, std::string dotReplacer) {
	std::string pattern = R"((http[s]?://)?([a-zA-Z0-9-]+\.)+[a-zA-Z]{2,6}(/[\w\-./?%&=]*)?|([a-zA-Z0-9_.+-]+@[a-zA-Z0-9-]+\.[a-zA-Z0-9-.]+))";
	std::regex regex(pattern);

	std::string result;
	std::sregex_iterator begin(text.begin(), text.end(), regex);
	std::sregex_iterator end;

	size_t lastPos = 0;

	for (auto it = begin; it != end; ++it) {
		auto matchPos = it->position();
		auto matchLen = it->length();

		// Append the part before the match
		result += text.substr(lastPos, matchPos - lastPos);

		// Censor the match
		std::string censored = it->str();
		size_t pos = 0;
		while ((pos = censored.find(".", pos)) != std::string::npos) {
			censored.replace(pos, 1, dotReplacer);
			pos += dotReplacer.length(); // move past the inserted text
		}
		result += censored;

		lastPos = matchPos + matchLen;
	}

	// Append the rest of the string after the last match
	result += text.substr(lastPos);

	return result;
}

void dPlayerControl_RpcSendChat(PlayerControl* __this, String* chatText, MethodInfo* method)
{
	if (State.ShowHookLogs) Log.HookDebug("Hook dPlayerControl_RpcSendChat executed", false);
	PlayerControl_RpcSendChat(__this, chatText, NULL); // This hook should be useless since dChatController_SendFreeChat sends rpc directly
}

void dChatBubble_SetText(ChatBubble* __this, String* chatText, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dChatBubble_SetText executed", false);
	if ((!State.PanicMode || State.TempPanicMode)) {
		// std::string colorOpener = "", colorCloser = "";
		SpriteRenderer_set_color(__this->fields.MaskArea, Palette__TypeInfo->static_fields->ClearWhite, NULL);

		if (State.CustomGameTheme) {
			auto bg32 = Color32();
			bg32.r = int(State.GameBgColor.x * 255); bg32.g = int(State.GameBgColor.y * 255); bg32.b = int(State.GameBgColor.z * 255); bg32.a = 255;
			auto bg = Color32_op_Implicit_1(bg32, NULL);
			auto text32 = Color32();
			text32.r = int(State.GameTextColor.x * 255); text32.g = int(State.GameTextColor.y * 255); text32.b = int(State.GameTextColor.z * 255); text32.a = 255;
			auto textCol = Color32_op_Implicit_1(text32, NULL);
			bool isChatWarning = __this->fields.playerInfo == NULL;
			if (!isChatWarning && __this->fields.playerInfo->fields.IsDead) bg.a *= 0.75f;
			SpriteRenderer_set_color(__this->fields.Background, bg, NULL);
			if (!isChatWarning) {
				auto textArea = __this->fields.TextArea;
				TMP_Text_set_color((app::TMP_Text*)textArea, textCol, NULL);
				// colorOpener = std::format("<#{:02x}{:02x}{:02x}>", text32.r, text32.g, text32.b);
				// colorCloser = "</color>";
			}
		}
		else if (State.DarkMode) {
			auto black = Color(0.133f, 0.133f, 0.133f, 1.f);
			bool isChatWarning = __this->fields.playerInfo == NULL;
			if (!isChatWarning && __this->fields.playerInfo->fields.IsDead) black.a *= 0.75f;
			SpriteRenderer_set_color(__this->fields.Background, black, NULL);
			if (!isChatWarning) {
				auto textArea = __this->fields.TextArea;
				TMP_Text_set_color((app::TMP_Text*)textArea, Palette__TypeInfo->static_fields->White, NULL);
				// colorOpener = "<#fff>";
				// colorCloser = "</color>";
			}
		}

		if (State.IsProcessingSickoChat) {
			auto darkGold = Color(0.6f, 0.4f, 0.f, 1.f);
			if (__this->fields.playerInfo->fields.IsDead) darkGold.a *= 0.75f;
			SpriteRenderer_set_color(__this->fields.Background, darkGold, NULL);
			TMP_Text_set_color((app::TMP_Text*)__this->fields.TextArea, Palette__TypeInfo->static_fields->White, NULL);
		}

		std::string fontOpener = "", fontCloser = "";
		if (State.ChatFont) {
			switch (State.ChatFontType) {
			case 0: {
				fontOpener = "<font=\"Barlow-Italic SDF\">";
				break;
			}
			case 1: {
				fontOpener = "<font=\"Barlow-Medium SDF\">";
				break;
			}
			case 2: {
				fontOpener = "<font=\"Barlow-Bold SDF\">";
				break;
			}
			case 3: {
				fontOpener = "<font=\"Barlow-SemiBold SDF\">";
				break;
			}
			case 4: {
				fontOpener = "<font=\"Barlow-SemiBold Masked\">";
				break;
			}
			case 5: {
				fontOpener = "<font=\"Barlow-ExtraBold SDF\">";
				break;
			}
			case 6: {
				fontOpener = "<font=\"Barlow-BoldItalic SDF\">";
				break;
			}			case 7: {
				fontOpener = "<font=\"Barlow-BoldItalic Masked\">";
				break;
			}
			case 8: {
				fontOpener = "<font=\"Barlow-Black SDF\">";
				break;
			}
			case 9: {
				fontOpener = "<font=\"Barlow-Light SDF\">";
				break;
			}
			case 10: {
				fontOpener = "<font=\"Barlow-Regular SDF\">";
				break;
			}
			case 11: {
				fontOpener = "<font=\"Barlow-Regular Masked\">";
				break;
			}
			case 12: {
				fontOpener = "<font=\"Barlow-Regular Outline\">";
				break;
			}
			case 13: {
				fontOpener = "<font=\"Brook SDF\">";
				break;
			}
			case 14: {
				fontOpener = "<font=\"LiberationSans SDF\">";
				break;
			}
			case 15: {
				fontOpener = "<font=\"NotoSansJP-Regular SDF\">";
				break;
			}
			case 16: {
				fontOpener = "<font=\"VCR SDF\">";
				break;
			}
			case 17: {
				fontOpener = "<font=\"CONSOLA SDF\">";
				break;
			}
			case 18: {
				fontOpener = "<font=\"digital-7 SDF\">";
				break;
			}
			case 19: {
				fontOpener = "<font=\"OCRAEXT SDF\">";
				break;
			}
			case 20: {
				fontOpener = "<font=\"DIN_Pro_Bold_700 SDF\">";
				break;
			}
			}

			fontCloser = "</font>";
		}

		chatText = convert_to_string(fontOpener + /*colorOpener + */convert_from_string(chatText) + /*colorCloser + */fontCloser);
	}
	ChatBubble_SetText(__this, chatText, method);
}

void dChatController_SendFreeChat(ChatController* __this, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dChatController_SendFreeChat executed", false);
	auto chatText = convert_from_string(__this->fields.freeChatField->fields.textArea->fields.text);
	if (convert_to_string(UncensorLink(chatText, ".­"))->fields.m_stringLength > 120) chatText = UncensorLink(chatText);
	else chatText = UncensorLink(chatText, ".­");
	if (chatText == "") return;
	std::string chatTextLower = strToLower(chatText);
	if (!State.PanicMode) {
		State.WasPreviousMessageCommand = true;
		auto playerToChatAs = (!State.SafeMode && State.activeChatSpoof && State.playerToChatAs.has_value()) ? State.playerToChatAs.validate().get_PlayerControl() : *Game::pLocalPlayer;
		if (State.ReadAndSendSickoChat && chatTextLower.substr(0, 4) == "/sc " && chatText.substr(4) != "") {
			if (IsInGame()) State.rpcQueue.push(new RpcForceSickoChat(PlayerSelection(playerToChatAs), chatText.substr(4), true));
			if (IsInLobby()) State.lobbyRpcQueue.push(new RpcForceSickoChat(PlayerSelection(playerToChatAs), chatText.substr(4), true));
			return; //we don't want the chat to know we're using "aum"
		}

		// Utility commands are local-only. Their output is sent publicly by this client.
		if (chatText[0] == '/') {
			size_t commandEnd = chatText.find_first_of(" \t");
			std::string command = strToLower(chatText.substr(0, commandEnd));
			if (command == "/tl" || command == "/rnd" || command == "/selfinf" || command == "/ai") {
				HandleSickoSocialCommand(*Game::pLocalPlayer, chatText);
				return;
			}
		}

		// /8ball is handled by the player who invokes it, so it works in public
		// rooms even when that player is not the host.
		if (chatTextLower == "/8ball" || chatTextLower.rfind("/8ball ", 0) == 0) {
			if (HandleSickoSocialCommand(*Game::pLocalPlayer, chatText)) return;
		}

		if (IsHost() && chatText[0] == '/') {
			if (HandleChatCommand(*Game::pLocalPlayer, chatText)) return;
		}

		// Added as an extension to the "Whitelisted Players Only" feature and other commands
		if (State.ExtraCommands) {

			if (chatTextLower == "/add" || chatTextLower == "/add ") {
				std::string msg = "<#aaaaaa><size=-0.24><font=\"Barlow-Regular Masked\"><b>Usage: /add <FriendCode></b></font></color>";
				ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string(msg), NULL);
				return;
			}

			if (chatTextLower.substr(0, 5) == "/add ") {
				std::string fc = chatText.substr(5);
				if (!fc.empty()) {
					if (std::find(State.WhitelistFriendCodes.begin(), State.WhitelistFriendCodes.end(), fc) == State.WhitelistFriendCodes.end()) {
						State.WhitelistFriendCodes.push_back(fc);

						std::string msg = std::format("<#5cff83><size=-0.24><font=\"Barlow-Regular Masked\"><b>\"{}\" Added to Whitelist.</b></font></color>", fc);
						ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string(msg), NULL);
					}
					else {
						std::string msg = std::format("<#ffd93d><size=-0.24><font=\"Barlow-Regular Masked\"><b>\"{}\" Already in Whitelist.</b></font></color>", fc);
						ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string(msg), NULL);
					}
				}
				return;
			}



			if (chatTextLower == "/remove" || chatTextLower == "/remove ") {
				std::string msg = "<#aaaaaa><size=-0.24><font=\"Barlow-Regular Masked\"><b>Usage: /remove <FriendCode></b></font></color>";
				ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string(msg), NULL);
				return;
			}

			if (chatTextLower.substr(0, 8) == "/remove ") {
				std::string fc = chatText.substr(8);
				auto it = std::find(State.WhitelistFriendCodes.begin(), State.WhitelistFriendCodes.end(), fc);
				if (it != State.WhitelistFriendCodes.end()) {
					State.WhitelistFriendCodes.erase(it);

					std::string msg = std::format("<#ff5c5c><size=-0.24><font=\"Barlow-Regular Masked\"><b>\"{}\" Removed from Whitelist.</b></font></color>", fc);
					ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string(msg), NULL);
				}
				else {
					std::string msg = std::format("<#ff0000><size=-0.24><font=\"Barlow-Regular Masked\"><b>\"{}\" Not found in Whitelist.</b></font></color>", fc);
					ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string(msg), NULL);
				}
				return;
			}



			if (IsHost()) {
				if (chatTextLower == "/kick all") {
					bool isBan = false;
					if (IsInGame()) {
						State.rpcQueue.push(new PunishEveryone(isBan));
					}					else if (IsInLobby()) {
						State.lobbyRpcQueue.push(new PunishEveryone(isBan));
					}
					return;
				}

				if (chatTextLower == "/ban all") {
					bool isBan = true;
					if (IsInGame()) {
						State.rpcQueue.push(new PunishEveryone(isBan));
					}
					else if (IsInLobby()) {
						State.lobbyRpcQueue.push(new PunishEveryone(isBan));
					}
					return;
				}
			}

			// without temp-ban command due to runtime error + uncomfortable use :despair:
		}

		if (State.WasPreviousMessageCommand) State.WasPreviousMessageCommand = false;
		// this was done to avoid repeating (State.WasPreviousMessageCommand = true) before every return statement

		if (State.CurrentChatMode == QuickChatModes__Enum::QuickChatOnly) {
			ChatController_AddChatWarning(Game::HudManager.GetInstance()->fields.Chat, convert_to_string("Free chat is not allowed!"), NULL);
			return;
		}

		if (State.activeWhisper && State.playerToWhisper.has_value()) {
			MessageWriter* writer = InnerNetClient_StartRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient),
				playerToChatAs->fields._.NetId, uint8_t(RpcCalls__Enum::SendChat), SendOption__Enum::Reliable,
				State.playerToWhisper.get_PlayerControl().value_or(nullptr)->fields._.OwnerId, NULL);
			std::string whisperMsg = std::format("{} whispers to you:\n{}",
				RemoveHtmlTags(convert_from_string(NetworkedPlayerInfo_get_PlayerName(GetPlayerData(*Game::pLocalPlayer), NULL))),
				chatText);
			if (whisperMsg.length() <= 100 || !State.SafeMode)
				MessageWriter_WriteString(writer, convert_to_string(whisperMsg), NULL);
			else MessageWriter_WriteString(writer, convert_to_string(chatText), NULL);
			InnerNetClient_FinishRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient), writer, NULL);

			std::string whisperMsgSelf = std::format("You whisper to {}:\n{}",
				RemoveHtmlTags(convert_from_string(NetworkedPlayerInfo_get_PlayerName(State.playerToWhisper.get_PlayerData().value_or(nullptr), NULL))),
				chatText);
			dChatController_AddChat(Game::HudManager.GetInstance()->fields.Chat, playerToChatAs, convert_to_string(whisperMsgSelf), false, NULL);
		}
		else if (!State.SafeMode && State.activeChatSpoof && State.playerToChatAs.has_value()) {
			auto writer = InnerNetClient_StartRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient), GetPlayerControlById(State.playerToChatAs.get_PlayerId())->fields._.NetId,
				uint8_t(RpcCalls__Enum::SendChat), SendOption__Enum::Reliable, -1, NULL);
			MessageWriter_WriteString(writer, convert_to_string(chatText), NULL);
			InnerNetClient_FinishRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient), writer, NULL);
			dChatController_AddChat(Game::HudManager.GetInstance()->fields.Chat, GetPlayerControlById(State.playerToChatAs.get_PlayerId()), convert_to_string(chatText), false, NULL);
		}
		else {
			auto writer = InnerNetClient_StartRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient), (*Game::pLocalPlayer)->fields._.NetId,
				uint8_t(RpcCalls__Enum::SendChat), SendOption__Enum::Reliable, -1, NULL);
			MessageWriter_WriteString(writer, convert_to_string(chatText), NULL);
			InnerNetClient_FinishRpcImmediately((InnerNetClient*)(*Game::pAmongUsClient), writer, NULL);
			dChatController_AddChat(Game::HudManager.GetInstance()->fields.Chat, *Game::pLocalPlayer, convert_to_string(chatText), false, NULL);
		}
	}
	else {
		if (State.CurrentChatMode == QuickChatModes__Enum::QuickChatOnly) return;
		PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(chatText), NULL);
	}
}

void dChatNotification_SetUp(ChatNotification* __this, PlayerControl* sender, String* text, MethodInfo* method) {
	if (!State.PanicMode && State.BetterChatNotifications) {
		ShowChatNotification(__this, sender, text);
		return;
	}
	ChatNotification_SetUp(__this, sender, text, method);
	ChangeChatNotificationBackground(__this, sender);
}

void dFreeChatInputField_UpdateCharCount(FreeChatInputField* __this, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dFreeChatInputField_UpdateCharCount executed", false);
	FreeChatInputField_UpdateCharCount(__this, method);
	__this->fields.charCountText->fields._.m_enableWordWrapping = false;

	updateCharCounterText(__this);
}

void dObjectPoolBehavior_InitPool(ObjectPoolBehavior* __this, PoolableBehavior* prefab, MethodInfo* method) {
	ObjectPoolBehavior_InitPool(__this, prefab, method);
}

AudioSource* dSoundManager_PlaySound(SoundManager* __this, AudioClip* clip, bool loop, float volume, AudioMixerGroup* audioMixer, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dSoundManager_PlaySound executed", false);
	// if (State.BetterMessageSounds && State.MessageSound != NULL && clip == State.MessageSound) volume = 0.f;
	return SoundManager_PlaySound(__this, clip, loop, volume, audioMixer, method);
}

void dChatController_Toggle(ChatController* __this, MethodInfo* method) {
	auto hud = Game::HudManager.GetInstance();
	auto chatState = hud->fields.Chat->fields.state;
	bool isOpenOrOpening = chatState == ChatControllerState__Enum::Open ||
		chatState == ChatControllerState__Enum::Opening;

	if (State.FollowerCam != NULL && Camera_get_orthographicSize(State.FollowerCam, NULL) != 3.f &&
		!isOpenOrOpening) {
		Camera_set_orthographicSize(State.FollowerCam, 3.f, NULL);
		Camera_set_orthographicSize(Game::HudManager.GetInstance()->fields.UICamera, 3.f, NULL);
		State.HasRefreshedUI = false;
	}

	if (isOpenOrOpening) {
		auto hudGameObject = Component_get_gameObject((Component_1*)Game::HudManager.GetInstance(), NULL);
		auto fullScreen = hud->fields.FullScreen;
		Color fullScreenCol = fullScreen != NULL ? SpriteRenderer_get_color(fullScreen, NULL) : Color(1.f, 1.f, 1.f, 0.f);
		bool isFullScreenActive = fullScreen != NULL &&
			fullScreenCol.r == 0.f && fullScreenCol.g == 0.f && fullScreenCol.b == 0.f &&
			GameObject_GetActive(Component_get_gameObject((Component_1*)fullScreen, NULL), NULL);
		bool shouldEnableZoom = (!State.InMeeting && !State.InExileUI && !isFullScreenActive && (State.GameLoaded || IsInLobby()) && !State.PanicMode);

		if (hudGameObject != NULL && isOpenOrOpening && shouldEnableZoom) {
			GameObject_SetActive(hudGameObject, false, NULL);
			GameObject_SetActive(hudGameObject, true, NULL);
			// restore the HUD after closing the chat UI
		}
	}
	ChatController_Toggle(__this, method);
}