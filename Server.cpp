#include "pch.h"
#include "Server.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>
#include "capnpdata/data.hpp"

#pragma comment(lib, "ws2_32.lib") // Link with ws2_32.lib

BAKKESMOD_PLUGIN(MAIServer, "MAI server plugin", plugin_version, PLUGINTYPE_BOTAI)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

void MAIServer::onLoad()
{
	_globalCvarManager = cvarManager;

	initServer();
	server_thread = new std::thread([this] { serveThread(); });

	gameWrapper->HookEventWithCaller<CarWrapper>(
		"Function TAGame.Car_TA.SetVehicleInput",
		[this](CarWrapper caller, void* params, std::string eventName) {
			if (client_socket == INVALID_SOCKET) return;
			ControllerInput* input = static_cast<ControllerInput*>(params);
			input->Throttle = latest_controls.getThrottle();
			input->Steer = latest_controls.getSteer();
			input->Pitch = latest_controls.getPitch();
			input->Yaw = latest_controls.getYaw();
			input->Roll = latest_controls.getRoll();
			if (!input->ActivateBoost) {
				input->ActivateBoost = latest_controls.getBoost();
			}
			input->HoldingBoost = latest_controls.getBoost();
			if (!input->Jump) {
				input->Jump = latest_controls.getJump();
			}
			input->Jumped = latest_controls.getJump();
			input->DodgeForward = latest_controls.getDodgeForward();
			input->DodgeStrafe = latest_controls.getDodgeStrafe();
		});

	// !! Enable debug logging by setting DEBUG_LOG = true in logging.h !!
	//DEBUGLOG("Server debug mode enabled");

	// LOG and DEBUGLOG use fmt format strings https://fmt.dev/latest/index.html
	//DEBUGLOG("1 = {}, 2 = {}, pi = {}, false != {}", "one", 2, 3.14, true);

	//cvarManager->registerNotifier("my_aweseome_notifier", [&](std::vector<std::string> args) {
	//	LOG("Hello notifier!");
	//}, "", 0);

	//auto cvar = cvarManager->registerCvar("template_cvar", "hello-cvar", "just a example of a cvar");
	//auto cvar2 = cvarManager->registerCvar("template_cvar2", "0", "just a example of a cvar with more settings", true, true, -10, true, 10 );

	//cvar.addOnValueChanged([this](std::string cvarName, CVarWrapper newCvar) {
	//	LOG("the cvar with name: {} changed", cvarName);
	//	LOG("the new value is: {}", newCvar.getStringValue());
	//});

	//cvar2.addOnValueChanged(std::bind(&Server::YourPluginMethod, this, _1, _2));

	// enabled decleared in the header
	//enabled = std::make_shared<bool>(false);
	//cvarManager->registerCvar("TEMPLATE_Enabled", "0", "Enable the TEMPLATE plugin", true, true, 0, true, 1).bindTo(enabled);

	//cvarManager->registerNotifier("NOTIFIER", [this](std::vector<std::string> params){FUNCTION();}, "DESCRIPTION", PERMISSION_ALL);
	//cvarManager->registerCvar("CVAR", "DEFAULTVALUE", "DESCRIPTION", true, true, MINVAL, true, MAXVAL);//.bindTo(CVARVARIABLE);
	//gameWrapper->HookEvent("FUNCTIONNAME", std::bind(&TEMPLATE::FUNCTION, this));
	//gameWrapper->HookEventWithCallerPost<ActorWrapper>("FUNCTIONNAME", std::bind(&Server::FUNCTION, this, _1, _2, _3));
	//gameWrapper->RegisterDrawable(bind(&TEMPLATE::Render, this, std::placeholders::_1));

	//gameWrapper->HookEvent("Function TAGame.Ball_TA.Explode", [this](std::string eventName) {
	//	LOG("Your hook got called and the ball went POOF");
	//});
	// You could also use std::bind here
	//gameWrapper->HookEvent("Function TAGame.Ball_TA.Explode", std::bind(&Server::YourPluginMethod, this);
}

void MAIServer::onUnload()
{
	stop_server.store(true);
	if (server_thread) {
		server_thread = nullptr;
	}
	if (client_socket != INVALID_SOCKET) {
		closesocket(server_socket);
	}
	if (server_socket) {
		closesocket(server_socket);
		WSACleanup();
	}
}

int MAIServer::initServer()
{
	// Initialize Winsock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		LOG("WSAStartup failed: {}", WSAGetLastError());
		return 1;
	}

	// Create socket
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket == INVALID_SOCKET) {
		LOG("Socket creation failed: {}", WSAGetLastError());
		WSACleanup();
		return 2;
	}

	// Set the socket to non-blocking mode
	u_long mode = 1; // 1 to enable non-blocking socket
	if (ioctlsocket(server_socket, FIONBIO, &mode) != 0) {
		LOG("Failed to set non-blocking mode: {}", WSAGetLastError());
		closesocket(server_socket);
		WSACleanup();
		return 3;
	}

	// Bind socket to port
	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(DEFAULT_PORT);

	if (bind(server_socket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
		LOG("Bind failed: {}", WSAGetLastError());
		closesocket(server_socket);
		server_socket = INVALID_SOCKET;
		WSACleanup();
		return 4;
	}

	// Listen for incoming connections
	if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR) {
		LOG("Listen failed: {}", WSAGetLastError());
		closesocket(server_socket);
		server_socket = INVALID_SOCKET;
		WSACleanup();
		return 5;
	}

	return 0;
}

void MAIServer::serveThread()
{
	stop_server.store(false);
	sockaddr_in clientAddr;
	int clientAddrSize = sizeof(clientAddr);
	while (!stop_server.load()) {
		client_socket = accept(server_socket, (sockaddr*)&clientAddr, &clientAddrSize);
		if (client_socket == INVALID_SOCKET) {
			int error = WSAGetLastError();
			if (error == WSAEWOULDBLOCK) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			} else if (error == 10038) { // Server socket closed
				return;
			} else {
				closesocket(client_socket);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
		}
		while (!stop_server.load()) {
			if (!(
				gameWrapper->IsInOnlineGame()
				||
				gameWrapper->IsInCustomTraining()
				||
				gameWrapper->IsInFreeplay()
			)) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}
			auto state = collectGameState();
			send(client_socket, (const char*)state.begin(), state.size() * sizeof(capnp::word), 0);
			char buffer[MAICONTROLS_SIZE];
			auto receive_start = std::chrono::system_clock::now();
			int bytesReceived = -1;
			while (
			(
				std::chrono::system_clock::now()
				-
				receive_start
				) < RECEIVE_TIMEOUT_SECONDS
			) {
				bytesReceived = recv(client_socket, buffer, sizeof(buffer), 0);
				if (bytesReceived == -1) {
					//std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}
				break;
			}
			if (bytesReceived == -1) {
				closesocket(client_socket);
				break;
			} else if (bytesReceived > 40) {
				LOG("Awaited 40 bytes, but received {}", bytesReceived);
			} else if (bytesReceived == 0) {
				break;
			}
			capnp::FlatArrayMessageReader messageReader(
				kj::arrayPtr(
					reinterpret_cast<const capnp::word*>(buffer),
					bytesReceived / sizeof(capnp::word)
				)
			);
			MAIControls::Reader controls = messageReader.getRoot<MAIControls>();
			applyControls(controls);
		}
	}
}

void MAIServer::fill(Vector bm_vector, MAIVector::Builder builder)
{
	builder.setX(bm_vector.X);
	builder.setY(bm_vector.Y);
	builder.setZ(bm_vector.Z);
}

void MAIServer::fill(Rotator rotator, MAIRotator::Builder builder)
{
	builder.setPitch(rotator.Pitch);
	builder.setRoll(rotator.Roll);
	builder.setYaw(rotator.Yaw);
}

void MAIServer::fill(CarWrapper car, MAIRLObjectState::Builder builder)
{
	fill(car.GetLocation(), builder.initPosition());
	fill(car.GetRotation(), builder.initRotation());
	fill(car.GetAngularVelocity(), builder.initAngularVelocity());
	fill(car.GetVelocity(), builder.initVelocity());
}

void MAIServer::fill(BallWrapper ball, MAIRLObjectState::Builder builder)
{
	fill(ball.GetLocation(), builder.initPosition());
	fill(ball.GetRotation(), builder.initRotation());
	fill(ball.GetAngularVelocity(), builder.initAngularVelocity());
	fill(ball.GetVelocity(), builder.initVelocity());
}


kj::Array<capnp::word> MAIServer::collectGameState()
{
	::capnp::MallocMessageBuilder message;
	auto game_state = message.initRoot<MAIGameState>();
	if (gameWrapper == nullptr) {
		LOG("GameWrapper is null");
		return capnp::messageToFlatArray(message);
	}
	game_state.setDead(true); // TODO
	data_collected.store(false);
	gameWrapper->Execute([this, &game_state](GameWrapper* gw) {
		// RL car
		CarWrapper car = gw->GetLocalCar();
		if (!car) {
		} else if (car.IsNull()) {
		} else {
			auto MAICar = game_state.initCar();
			fill(car, MAICar);
			game_state.setBoostAmount(car.GetBoostComponent().GetCurrentBoostAmount());
			game_state.setDead(false);
		}

		// RL server
		ServerWrapper server = gw->GetCurrentGameState();
		if (!server) {
		} else if (server.IsNull()) {
		} else {
			// RL ball
			BallWrapper ball = server.GetBall();
			if (!ball) {
				return;
			}
			if (ball.IsNull()) {
				return;
			}
			else {
				fill(ball, game_state.initBall());
			}
		}

		// ArrayWrapper<CarWrapper> cars(server.GetCars());
		data_collected.store(true);
	});

	while (!data_collected.load()) {
		continue;
	}

	return capnp::messageToFlatArray(message);
}

void MAIServer::applyControls(MAIControls::Reader reader) { latest_controls = reader; }
