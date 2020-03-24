#include "Player.hpp"
#include "CommandQueue.hpp"
#include "Aircraft.hpp"
#include "ActionID.hpp"
#include "NetworkProtocol.hpp"
#include "GameActionID.hpp"

#include <SFML/Network/Packet.hpp>

#include <map>
#include <string>
#include <algorithm>
#include <iostream>


struct AircraftMover
{
	AircraftMover(float vx, float vy, int identifier)
		: velocity(vx, vy), aircraftID(identifier)
	{
	}

	void operator() (Aircraft& aircraft, sf::Time) const
	{
		if (aircraft.getIdentifier() == aircraftID)
		{
			aircraft.accelerate(velocity * aircraft.getMaxSpeed());
		}
	}

	sf::Vector2f velocity;
	int aircraftID;
};

struct AircraftFireTrigger
{
	AircraftFireTrigger(int identifier)
		: aircraftID(identifier)
	{
	}

	void operator() (Aircraft& aircraft, sf::Time) const
	{
		if (aircraft.getIdentifier() == aircraftID)
			aircraft.fire();
	}

	int aircraftID;
};

struct AircraftMissileTrigger
{
	AircraftMissileTrigger(int identifier)
		: aircraftID(identifier)
	{
	}

	void operator() (Aircraft& aircraft, sf::Time) const
	{
		if (aircraft.getIdentifier() == aircraftID)
			aircraft.launchMissile();
	}

	int aircraftID;
};

Player::Player(sf::TcpSocket* socket, sf::Int32 identifier, const KeyBinding* binding) : mKeyBinding(binding), mCurrentMissionStatus(MissionStatusID::MissionRunning), mIdentifier(identifier), mSocket(socket)
{
	// Set initial action bindings
	initializeActions();

	// Assign all categories to player's aircraft
	for (auto& pair : mActionBinding)
		pair.second.category = static_cast<int>(CategoryID::PlayerAircraft);
}

void Player::handleEvent(const sf::Event& event, CommandQueue& commands)
{
	if (event.type == sf::Event::KeyPressed)
	{
		// Check if pressed key appears in key binding, trigger command if so
		ActionID action;
		if (mKeyBinding && mKeyBinding->checkAction(event.key.code, action) && !isRealtimeAction(action))
		{
			// Network connected -> send event over network
			if (mSocket)
			{
				sf::Packet packet;
				packet << static_cast<sf::Int32>(Client::PacketType::PlayerEvent);
				packet << mIdentifier;
				packet << static_cast<sf::Int32>(action);
				mSocket->send(packet);
			}
			else
			{
				commands.push(mActionBinding[action]);
			}
		}
	}
}

bool Player::isLocal() const
{
	// No key binding means this player is remote
	return mKeyBinding != nullptr;
}

void Player::disableAllRealtimeActions()
{
	for (auto& action : mActionProxies)
	{
		sf::Packet packet;
		packet << static_cast<sf::Int32>(Client::PacketType::PlayerRealtimeChange);
		packet << mIdentifier;
		packet << static_cast<sf::Int32>(action.first);
		packet << false;
		mSocket->send(packet);
	}
}

void Player::handleRealtimeInput(CommandQueue& commands)
{
	// Check if this is a networked game and local player or just a single player game
	if ((mSocket && isLocal()) || !mSocket)
	{
		// Lookup all actions and push corresponding commands to queue
		std::vector<ActionID> activeActions = mKeyBinding->getRealtimeActions();
		for (ActionID action : activeActions)
		{
			commands.push(mActionBinding[action]);
		}
	}
}

void Player::handleRealtimeNetworkInput(CommandQueue& commands)
{
	if (mSocket && !isLocal())
	{
		// Traverse all realtime input proxies. Because this is a networked game, the input isn't handled directly
		for (auto pair : mActionProxies)
		{
			if (pair.second && isRealtimeAction(pair.first))
				commands.push(mActionBinding[pair.first]);
		}
	}
}

void Player::handleNetworkEvent(ActionID action, CommandQueue& commands)
{
	commands.push(mActionBinding[action]);
}

void Player::handleNetworkRealtimeChange(ActionID action, bool actionEnabled)
{
	mActionProxies[action] = actionEnabled;
}

void Player::setMissionStatus(MissionStatusID status)
{
	mCurrentMissionStatus = status;
}

MissionStatusID Player::getMissionStatus() const
{
	return mCurrentMissionStatus;
}

void Player::initializeActions()
{
	mActionBinding[ActionID::MoveLeft].action = derivedAction<Aircraft>(AircraftMover(-1, 0, mIdentifier));
	mActionBinding[ActionID::MoveRight].action = derivedAction<Aircraft>(AircraftMover(+1, 0, mIdentifier));
	mActionBinding[ActionID::MoveUp].action = derivedAction<Aircraft>(AircraftMover(0, -1, mIdentifier));
	mActionBinding[ActionID::MoveDown].action = derivedAction<Aircraft>(AircraftMover(0, +1, mIdentifier));
	mActionBinding[ActionID::Fire].action = derivedAction<Aircraft>(AircraftFireTrigger(mIdentifier));
	mActionBinding[ActionID::LaunchMissile].action = derivedAction<Aircraft>(AircraftMissileTrigger(mIdentifier));
}
