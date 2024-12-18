#pragma once

#include <queue>
#include <mutex>

//#include "GuiBase.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include <winsock2.h>
#include <capnp/message.h>
#include <kj/io.h>
#include <kj/array.h>
#include "capnpdata/data.hpp"

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);

constexpr auto DEFAULT_PORT = 11545;
constexpr auto MAICONTROLS_SIZE = 80;
constexpr auto RECEIVE_TIMEOUT_SECONDS = std::chrono::seconds(3);
constexpr float RENDER_TEXT_OFFSET = 30;
const Vector ARENA_SIZE = Vector(4096, 5120+880, 2044);
const float ROTATION_DIVIDER = 5.5f;
constexpr auto TEAM_CARS_MAXIMUM = 4;

class MAIServer : public BakkesMod::Plugin::BakkesModPlugin
	//,public SettingsWindowBase // Uncomment if you wanna render your own tab in the settings menu
	//,public PluginWindowBase // Uncomment if you want to render your own plugin window
{
	//std::shared_ptr<bool> enabled;
	std::thread* server_thread = nullptr;
	std::atomic_bool stop_server = false;
	std::atomic_bool data_collected = false;
	SOCKET server_socket = INVALID_SOCKET;
	SOCKET client_socket = INVALID_SOCKET;
	MAIControls::Reader latest_controls;
	Vector map_center = Vector(0, 0, 0);
	std::queue<MAIGameState::MessageType> messages;

	std::vector<CarWrapper> allies;
	std::vector<CarWrapper> enemies;

	int initServer();
	void performHooks();
	void serveThread();

	//Boilerplate
	void onLoad() override;
	void onUnload() override; // Uncomment and implement if you need a unload method
	void Render(CanvasWrapper) const;

	void fill(Vector, MAIVector::Builder, bool normalize = true);
	void fill(Rotator, MAIRotator::Builder);
	void fill(CarWrapper, MAIRLObjectState::Builder);
	void fill(BallWrapper, MAIRLObjectState::Builder);

	kj::Array<capnp::word> collectGameState();
	void applyControls(MAIControls::Reader);
	void RefreshTeamMembers();

public:
	//void RenderSettings() override; // Uncomment if you wanna render your own tab in the settings menu
	//void RenderWindow() override; // Uncomment if you want to render your own plugin window
};
