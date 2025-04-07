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
	allies .reserve(TEAM_CARS_MAXIMUM-1);
	enemies.reserve(TEAM_CARS_MAXIMUM);

	initServer();
	server_thread = new std::thread([this] { serveThread(); });
	performHooks();

	gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) {
		Render(canvas);
	});
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

void MAIServer::performHooks()
{
	gameWrapper->HookEventWithCaller<CarWrapper>(
		"Function TAGame.Car_TA.SetVehicleInput",
		[this](CarWrapper caller, void* params, std::string eventName) {
			if (client_socket == INVALID_SOCKET) return;
			if (latest_controls.getSkip()) return;
			if (caller.memory_address != gameWrapper->GetLocalCar().memory_address) return;
			ControllerInput* input = static_cast<ControllerInput*>(params);
			input->Throttle = latest_controls.getThrottle();
			input->Steer = latest_controls.getSteer();
			input->Pitch = latest_controls.getPitch();
			input->Yaw = latest_controls.getYaw();
			input->Roll = latest_controls.getRoll();
			input->DodgeForward = latest_controls.getDodgeForward();
			input->DodgeStrafe = latest_controls.getDodgeStrafe();

			if (!input->ActivateBoost) {
				input->ActivateBoost = latest_controls.getBoost();
			}
			input->HoldingBoost = latest_controls.getBoost();

			if (latest_controls.getJump()) {
				input->Jump = jump_switch;
				jump_switch = !jump_switch;
			}
		});
	gameWrapper->HookEventWithCallerPost<ServerWrapper>(
		"Function TAGame.GameEvent_Soccar_TA.Destroyed",
		[this](ServerWrapper caller, void* params, std::string eventname) {
			//ball_default_position = Vector();
			this->messages.push(MAIGameState::MessageType::GAME_EXIT);
		}
	);
	gameWrapper->HookEventWithCallerPost<BallWrapper>(
		"Function TAGame.Ball_TA.Explode",
		[this](BallWrapper caller, void* params, std::string eventname) {
			this->messages.push(MAIGameState::MessageType::BALL_EXPLODE);
		}
	);
	gameWrapper->HookEventPost(
		"Function TAGame.StatusObserver_Products_TA.OnTeamChanged",
		[this](std::string eventname) {
			this->RefreshTeamMembers();
		}
	);
	gameWrapper->HookEventWithCallerPost<ServerWrapper>(
		"Function GameEvent_Soccar_TA.Countdown.BeginState",
		[this](ServerWrapper caller, void* params, std::string eventname) {
			/*gameWrapper->SetTimeout([this](GameWrapper* gw) {
				this->ball_default_position = gw->GetCurrentGameState().GetBall().GetLocation();
				this->ball_default_position.Z -= 40;
			}, 1.f);*/
			this->RefreshTeamMembers();
			this->messages.push(MAIGameState::MessageType::KICKOFF_TIMER_STARTED);
		}
	);
	gameWrapper->HookEventPost(
		"Function GameEvent_Soccar_TA.Active.StartRound",
		[this](std::string eventname) {
			this->messages.push(MAIGameState::MessageType::KICKOFF_TIMER_ENDED);
		}
	);
	gameWrapper->HookEventPost(
		"Function GameEvent_Soccar_TA.ReplayPlayback.BeginState",
		[this](std::string eventname) {
			this->messages.push(MAIGameState::MessageType::REPLAY_STARTED);
		}
	);
	gameWrapper->HookEventWithCallerPost<BallWrapper>(
		"Function TAGame.Ball_TA.OnCarTouch",
		[this](BallWrapper caller, void* params, std::string eventname) {
			// Skips adding ball touch if another event already in there
			// This line added bcz there's a more important messages than ball touch,
			// so during dribling or similiar context we can skip ball touches
			if (!messages.empty()) return;
			this->messages.push(MAIGameState::MessageType::BALL_TOUCHED);
		}
	);
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
				gameWrapper->IsInCustomTraining()
				||
				gameWrapper->IsInFreeplay()
				||
				gameWrapper->IsInGame()
			)) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
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
					continue;
				}
				break;
			}
			if (bytesReceived == -1) {
				closesocket(client_socket);
				break;
			} else if (bytesReceived > 48) {
				LOG("Awaited 48 bytes, but received {}", bytesReceived);
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

void MAIServer::fill(Vector bm_vector, MAIVector::Builder builder, bool normalize)
{
	if (normalize) {
		builder.setX((bm_vector.X - map_center.X) / ARENA_SIZE.X);
		builder.setY((bm_vector.Y - map_center.Y) / ARENA_SIZE.Y);
		builder.setZ((bm_vector.Z - map_center.Z) / ARENA_SIZE.Z);
	} else {
		builder.setX(bm_vector.X/ROTATION_DIVIDER);
		builder.setY(bm_vector.Y/ROTATION_DIVIDER);
		builder.setZ(bm_vector.Z/ROTATION_DIVIDER);
	}
}

void MAIServer::fill(Rotator rotator, MAIRotator::Builder builder)
{
	builder.setPitch(rotator.Pitch / 16364.0f);
	builder.setRoll(rotator.Roll / 32768.0f);
	builder.setYaw(rotator.Yaw / 32768.0f);
}

void MAIServer::fill(CarWrapper car, MAIRLObjectState::Builder builder)
{
	fill(car.GetLocation(), builder.initPosition());
	fill(car.GetRotation(), builder.initRotation());
	fill(car.GetVelocity(), builder.initVelocity());
	fill(car.GetAngularVelocity(), builder.initAngularVelocity(), false);
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
	if (!gameWrapper) {
		return capnp::messageToFlatArray(message);
	}
	game_state.setDead(true);
	data_collected.store(false);
	gameWrapper->Execute([this, &game_state](GameWrapper* gw) {
		if (!this->messages.empty()) {
			game_state.setMessage(this->messages.front());
			this->messages.pop();
		}

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
			} else if (ball.IsNull()) {
			} else {
				fill(ball, game_state.initBall());
			}
		}

		// Other cars
		size_t allies_size = this->allies.size();
		size_t enemies_size = this->enemies.size();

		if ((allies_size > 0) || (enemies_size > 0)) {
			MAIGameState::OtherCars::Builder other_cars = game_state.initOtherCars();
			if (allies_size > 0) {
				auto mai_allies = other_cars.initAllies(allies_size);
				for (int i = 0; i < allies_size; i++) {
					CarWrapper car = this->allies[i];
					if (car.IsNull()) continue;
					fill(car, mai_allies[i]);
				}
			}
			if (enemies_size > 0) {
				auto mai_enemies = other_cars.initEnemies(enemies_size);
				for (int i = 0; i < enemies_size; i++) {
					CarWrapper car = this->enemies[i];
					if (car.IsNull()) continue;
					fill(car, mai_enemies[i]);
				}
			}
		}

		data_collected.store(true);
	});

	while (!data_collected.load()) { continue; }

	return capnp::messageToFlatArray(message);
}

void MAIServer::applyControls(MAIControls::Reader reader) {
	latest_controls = reader;
}

void MAIServer::Render(CanvasWrapper canvas) const {
	// defines colors in RGBA 0-255
	canvas.SetColor(LinearColor{ 255 ,255, 255, 120 });

	canvas.SetPosition(Vector2F{ 100.0f, 180.0f });
	float y_offset = 0;

	/*char buf[100];
	canvas.SetPosition(Vector2F{ 100.0f, 180.0f + y_offset });
	snprintf(
		buf,
		sizeof(buf),
		"Ball: (% 06f, % 06f, % 06f)",
		ball_default_position.X,
		ball_default_position.Y,
		ball_default_position.Z
	);
	canvas.DrawString(buf, 2.0f, 2.0f);
	y_offset += RENDER_TEXT_OFFSET;*/
}

void MAIServer::RefreshTeamMembers()
{
	allies.clear();
	enemies.clear();
	if (!gameWrapper) return;
	ServerWrapper server = gameWrapper->GetCurrentGameState();
	if (!server) return;
	CarWrapper local_car = gameWrapper->GetLocalCar();
	if (!local_car) return;
	ArrayWrapper<CarWrapper> cars = server.GetCars();
	if (cars.IsNull()) return;
	for (CarWrapper car : cars) {
		if (!car) continue;
		if (car.GetTeamNum2() == local_car.GetTeamNum2()) {
			if (car.memory_address == local_car.memory_address) continue;
			allies.push_back(car);
		} else {
			enemies.push_back(car);
		}
	}
}
