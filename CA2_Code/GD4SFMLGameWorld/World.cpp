#include "World.hpp"
#include "ParticleID.hpp"
#include "ParticleNode.hpp"
#include "NetworkNode.hpp"

#include <SFML/Graphics/RenderWindow.hpp>



World::World(sf::RenderTarget& outputTarget, FontHolder& fonts, SoundPlayer& sounds, bool networked)
	: mTarget(outputTarget)
	, mSceneTexture()
	, mCamera(outputTarget.getDefaultView())
	, mFonts(fonts)
	, mSounds(sounds)
	, mTextures()
	, mSceneGraph()
	, mSceneLayers()
	, mWorldBounds(0.f, 0.f, mCamera.getSize().x, 5000.f)
	, mSpawnPosition(mCamera.getSize().x / 2.f, mWorldBounds.height - mCamera.getSize().y / 2.f)
	, mScrollSpeed(-50.f)
	, mScrollSpeedCompensation(1.f)
	, mPlayerAircraft()
	, mEnemySpawnPoints()
	, mActiveEnemies()
	, mNetworkedWorld(networked)
	, mNetworkNode(nullptr)
	, mFinishSprite(nullptr)
{
	mSceneTexture.create(mTarget.getSize().x, mTarget.getSize().y);
	loadTextures();
	buildScene();

	// Prepare the view
	mCamera.setCenter(mSpawnPosition);
}

void World::setWorldScrollCompensation(float compensation)
{
	mScrollSpeedCompensation = compensation;
}

void World::update(sf::Time dt)
{
	// Scroll the world, reset player velocity
	//mCamera.move(0.f, mScrollSpeed * dt.asSeconds()*mScrollSpeedCompensation);
	if (Aircraft* aircraft = getAircraft(1))
	{
		mCamera.setCenter(aircraft->getPosition());
	}

	for (Aircraft* a : mPlayerAircraft)
	{
		a->setVelocity(0.f, 0.f);
	}

	// Setup commands to destroy entities, and guide missiles
	destroyEntitiesOutsideView();
	guideMissiles();

	// Forward commands to scene graph, adapt velocity (scrolling, diagonal correction)
	while (!mCommandQueue.isEmpty())
		mSceneGraph.onCommand(mCommandQueue.pop(), dt);
	adaptPlayerVelocity();

	// Collision detection and response (may destroy entities)
	handleCollisions();

	// Remove aircrafts that were destroyed (World::removeWrecks() only destroys the entities, not the pointers in mPlayerAircraft)
	auto firstToRemove = std::remove_if(mPlayerAircraft.begin(), mPlayerAircraft.end(), std::mem_fn(&Aircraft::isMarkedForRemoval));
	mPlayerAircraft.erase(firstToRemove, mPlayerAircraft.end());

	// Remove all destroyed entities, create new ones
	mSceneGraph.removeWrecks();
	spawnEnemies();

	// Regular update step, adapt position (correct if outside view)
	mSceneGraph.update(dt, mCommandQueue);
	adaptPlayerPosition();

	updateSounds();
}

void World::draw()
{
	if (PostEffect::isSupported())
	{
		mSceneTexture.clear();
		mSceneTexture.setView(mCamera);
		mSceneTexture.draw(mSceneGraph);
		mSceneTexture.display();
		mBloomEffect.apply(mSceneTexture, mTarget);
	}
	else
	{
		mTarget.setView(mCamera);
		mTarget.draw(mSceneGraph);
	}
}

CommandQueue& World::getCommandQueue()
{
	return mCommandQueue;
}

Aircraft* World::getAircraft(int identifier) const
{
	for (Aircraft* a : mPlayerAircraft)
	{
		if (a->getIdentifier() == identifier)
			return a;
	}

	return nullptr;
}

void World::removeAircraft(int identifier)
{
	Aircraft* aircraft = getAircraft(identifier);
	if (aircraft)
	{
		aircraft->destroy();
		mPlayerAircraft.erase(std::find(mPlayerAircraft.begin(), mPlayerAircraft.end(), aircraft));
	}
}

Aircraft* World::addAircraft(int identifier)
{
	std::unique_ptr<Aircraft> player(new Aircraft(AircraftID::Eagle, mTextures, mFonts));
	player->setPosition(mCamera.getCenter());
	player->setIdentifier(identifier);

	mPlayerAircraft.push_back(player.get());
	mSceneLayers[static_cast<int>(LayerID::UpperAir)]->attachChild(std::move(player));
	return mPlayerAircraft.back();
}

void World::createPickup(sf::Vector2f position, PickupID type)
{
	std::unique_ptr<Pickup> pickup(new Pickup(type, mTextures));
	pickup->setPosition(position);
	pickup->setVelocity(0.f, 1.f);
	mSceneLayers[static_cast<int>(LayerID::UpperAir)]->attachChild(std::move(pickup));
}

bool World::pollGameAction(Action& out)
{
	return mNetworkNode->pollGameAction(out);
}

void World::setCurrentBattleFieldPosition(float lineY)
{
	mCamera.setCenter(mCamera.getCenter().x, lineY - mCamera.getSize().y / 2);
	mSpawnPosition.y = mWorldBounds.height;
}

void World::setWorldHeight(float height)
{
	mWorldBounds.height = height;
}

bool World::hasAlivePlayer() const
{
	return mPlayerAircraft.size() > 0;
}

bool World::hasPlayerReachedEnd() const
{
	if (Aircraft* aircraft = getAircraft(1))
	{
		return !mWorldBounds.contains(aircraft->getPosition());
	}
	else
	{
		return false;
	}
}

void World::updateSounds()
{
	sf::Vector2f listenerPosition;

	// 0 players (multiplayer mode, until server is connected) -> view center
	if (mPlayerAircraft.empty())
	{
		listenerPosition = mCamera.getCenter();
	}

	// 1 or more players -> mean position between all aircrafts
	else
	{
		for (Aircraft* aircraft : mPlayerAircraft)
			listenerPosition += aircraft->getWorldPosition();

		listenerPosition /= static_cast<float>(mPlayerAircraft.size());
	}
	//Set the listener to the player position
	mSounds.setListenPosition(listenerPosition);
	//Remove unused sounds
	mSounds.removeStoppedSounds();

}

void World::loadTextures()
{
	mTextures.load(TextureID::Entities, "Media/Textures/Entities.png");
	mTextures.load(TextureID::Jungle, "Media/Textures/Jungle.png");
	mTextures.load(TextureID::Explosion, "Media/Textures/Explosion.png");
	mTextures.load(TextureID::Particle, "Media/Textures/Particle.png");
	mTextures.load(TextureID::FinishLine, "Media/Textures/FinishLine.png");
}

bool matchesCategories(SceneNode::Pair& colliders, CategoryID type1, CategoryID type2)
{
	unsigned int category1 = colliders.first->getCategory();
	unsigned int category2 = colliders.second->getCategory();

	// Make sure first pair entry has category type1 and second has type2
	if (((static_cast<int>(type1))& category1) && ((static_cast<int>(type2))& category2))
	{
		return true;
	}
	else if (((static_cast<int>(type1))& category2) && ((static_cast<int>(type2))& category1))
	{
		std::swap(colliders.first, colliders.second);
		return true;
	}
	else
	{
		return false;
	}
}

void World::handleCollisions()
{
	std::set<SceneNode::Pair> collisionPairs;
	mSceneGraph.checkSceneCollision(mSceneGraph, collisionPairs);

	for (SceneNode::Pair pair : collisionPairs)
	{
		if (matchesCategories(pair, CategoryID::PlayerAircraft, CategoryID::EnemyAircraft))
		{
			auto& player = static_cast<Aircraft&>(*pair.first);
			auto& enemy = static_cast<Aircraft&>(*pair.second);

			// Collision: Player damage = enemy's remaining HP
			player.damage(enemy.getHitpoints());
			enemy.destroy();
		}

		else if (matchesCategories(pair, CategoryID::PlayerAircraft, CategoryID::Pickup))
		{
			auto& player = static_cast<Aircraft&>(*pair.first);
			auto& pickup = static_cast<Pickup&>(*pair.second);

			// Apply pickup effect to player, destroy projectile
			pickup.apply(player);
			player.playerLocalSound(mCommandQueue, SoundEffectID::CollectPickup);
			pickup.destroy();
		}

		else if (matchesCategories(pair, CategoryID::EnemyAircraft, CategoryID::AlliedProjectile)
			|| matchesCategories(pair, CategoryID::PlayerAircraft, CategoryID::EnemyProjectile))
		{
			auto& aircraft = static_cast<Aircraft&>(*pair.first);
			auto& projectile = static_cast<Projectile&>(*pair.second);

			// Apply projectile damage to aircraft, destroy projectile
			aircraft.damage(projectile.getDamage());
			projectile.destroy();
		}
	}
}

void World::buildScene()
{
	// Initialize the different layers
	for (std::size_t i = 0; i < static_cast<int>(LayerID::LayerCount); ++i)
	{
		CategoryID category = (i == (static_cast<int>(LayerID::LowerAir))) ? CategoryID::SceneAirLayer : CategoryID::None;

		SceneNode::Ptr layer(new SceneNode(category));
		mSceneLayers[i] = layer.get();

		mSceneGraph.attachChild(std::move(layer));
	}

	// Prepare the tiled background

	sf::Texture& texture = mTextures.get(TextureID::Jungle);
	sf::IntRect textureRect(mWorldBounds);
	texture.setRepeated(true);

	// Add the background sprite to the scene
	std::unique_ptr<SpriteNode> backgroundSprite(new SpriteNode(texture, textureRect));
	backgroundSprite->setPosition(mWorldBounds.left, mWorldBounds.top);
	mSceneLayers[static_cast<int>(LayerID::Background)]->attachChild(std::move(backgroundSprite));

	//Add the finish line to the scene
	sf::Texture& finishTexture = mTextures.get(TextureID::FinishLine);
	std::unique_ptr<SpriteNode> finishSprite(new SpriteNode(finishTexture));
	finishSprite->setPosition(0.f, -76.f);
	mSceneLayers[static_cast<int>(LayerID::Background)]->attachChild(std::move(finishSprite));

	//Add particle nodes for smoke and propellant
	std::unique_ptr<ParticleNode> smokeNode(new ParticleNode(ParticleID::Smoke, mTextures));
	mSceneLayers[static_cast<int>(LayerID::LowerAir)]->attachChild(std::move(smokeNode));

	std::unique_ptr<ParticleNode> propellantNode(new ParticleNode(ParticleID::Propellant, mTextures));
	mSceneLayers[static_cast<int>(LayerID::LowerAir)]->attachChild(std::move(propellantNode));

	//Add the sound effect node
	std::unique_ptr<SoundNode> soundNode(new SoundNode(mSounds));
	mSceneGraph.attachChild(std::move(soundNode));

	// Add network node, if necessary
	if (mNetworkedWorld)
	{
		std::unique_ptr<NetworkNode> networkNode(new NetworkNode());
		mNetworkNode = networkNode.get();
		mSceneGraph.attachChild(std::move(networkNode));
	}

	addEnemies();
}

void World::adaptPlayerPosition()
{
	// Keep player's position inside the screen bounds, at least borderDistance units from the border
	sf::FloatRect viewBounds = getViewBounds();
	const float borderDistance = 40.f;
	for (Aircraft* aircraft : mPlayerAircraft)
	{
		sf::Vector2f position = aircraft->getPosition();
		position.x = std::max(position.x, viewBounds.left + borderDistance);
		position.x = std::min(position.x, viewBounds.left + viewBounds.width - borderDistance);
		position.y = std::max(position.y, viewBounds.top + borderDistance);
		position.y = std::min(position.y, viewBounds.top + viewBounds.height - borderDistance);
		aircraft->setPosition(position);
	}
}

void World::adaptPlayerVelocity()
{
	for (Aircraft* aircraft : mPlayerAircraft)
	{
		sf::Vector2f velocity = aircraft->getVelocity();

		// If moving diagonally, reduce velocity (to have always same velocity)
		if (velocity.x != 0.f && velocity.y != 0.f)
			aircraft->setVelocity(velocity / std::sqrt(2.f));

		// Add scrolling velocity
		//aircraft->accelerate(0.f, mScrollSpeed);
	}
}

void World::addEnemies()
{
	if (mNetworkedWorld)
	{
		return;
	}
	// Add enemies to the spawn point container
	addEnemy(AircraftID::Raptor, 0.f, 500.f);
	addEnemy(AircraftID::Raptor, 0.f, 1000.f);
	addEnemy(AircraftID::Raptor, +100.f, 1150.f);
	addEnemy(AircraftID::Raptor, -100.f, 1150.f);
	addEnemy(AircraftID::Avenger, 70.f, 1500.f);
	addEnemy(AircraftID::Avenger, -70.f, 1500.f);

	addEnemy(AircraftID::Avenger, -70.f, 1710.f);
	addEnemy(AircraftID::Avenger, 70.f, 1700.f);
	addEnemy(AircraftID::Avenger, 30.f, 1850.f);
	addEnemy(AircraftID::Raptor, 300.f, 2200.f);
	addEnemy(AircraftID::Raptor, -300.f, 2200.f);
	addEnemy(AircraftID::Raptor, 0.f, 2200.f);
	addEnemy(AircraftID::Raptor, 0.f, 2500.f);
	addEnemy(AircraftID::Avenger, -300.f, 2700.f);
	addEnemy(AircraftID::Avenger, -300.f, 2700.f);
	addEnemy(AircraftID::Raptor, 0.f, 3000.f);
	addEnemy(AircraftID::Raptor, 250.f, 3250.f);
	addEnemy(AircraftID::Raptor, -250.f, 3250.f);
	addEnemy(AircraftID::Avenger, 0.f, 3500.f);
	addEnemy(AircraftID::Avenger, 0.f, 3700.f);
	addEnemy(AircraftID::Raptor, 0.f, 3800.f);
	addEnemy(AircraftID::Avenger, 0.f, 4000.f);
	addEnemy(AircraftID::Avenger, -200.f, 4200.f);
	addEnemy(AircraftID::Raptor, 200.f, 4200.f);
	addEnemy(AircraftID::Raptor, 0.f, 4400.f);

	// Sort all enemies according to their y value, such that lower enemies are checked first for spawning
	sortEnemies();
}

void World::sortEnemies()
{
	std::sort(mEnemySpawnPoints.begin(), mEnemySpawnPoints.end(), [](SpawnPoint lhs, SpawnPoint rhs)
	{
		return lhs.y < rhs.y;
	});
}

void World::addEnemy(AircraftID type, float relX, float relY)
{
	SpawnPoint spawn(type, mSpawnPosition.x + relX, mSpawnPosition.y - relY);
	mEnemySpawnPoints.push_back(spawn);
}

void World::spawnEnemies()
{
	// Spawn all enemies entering the view area (including distance) this frame
	while (!mEnemySpawnPoints.empty()
		&& mEnemySpawnPoints.back().y > getBattlefieldBounds().top)
	{
		SpawnPoint spawn = mEnemySpawnPoints.back();

		std::unique_ptr<Aircraft> enemy(new Aircraft(spawn.type, mTextures, mFonts));
		enemy->setPosition(spawn.x, spawn.y);
		int compass = (1 + rand() % 4);
		if (compass == 1)
		{
			enemy->setRotation(180.f);
		}
		else if (compass == 2)
		{
			enemy->setRotation(90.f);
		}
		else if (compass == 3)
		{
			enemy->setRotation(0.f);
		}
		else if (compass == 4)
		{
			enemy->setRotation(270.f);
		}

		if (mNetworkedWorld)
		{
			enemy->disablePickups();
		}

		mSceneLayers[static_cast<int>(LayerID::UpperAir)]->attachChild(std::move(enemy));

		// Enemy is spawned, remove from the list to spawn
		mEnemySpawnPoints.pop_back();
	}
}

void World::destroyEntitiesOutsideView()
{
	Command command;
	command.category = static_cast<int>(CategoryID::Projectile) | static_cast<int>(CategoryID::EnemyAircraft);
	command.action = derivedAction<Entity>([this](Entity& e, sf::Time)
	{
		if (!getBattlefieldBounds().intersects(e.getBoundingRect()))
			e.destroy();
	});

	mCommandQueue.push(command);
}

void World::guideMissiles()
{
	// Setup command that stores all enemies in mActiveEnemies
	Command enemyCollector;
	enemyCollector.category = static_cast<int>(CategoryID::EnemyAircraft);
	enemyCollector.action = derivedAction<Aircraft>([this](Aircraft& enemy, sf::Time)
	{
		if (!enemy.isDestroyed())
			mActiveEnemies.push_back(&enemy);
	});

	// Setup command that guides all missiles to the enemy which is currently closest to the player
	Command missileGuider;
	missileGuider.category = static_cast<int>(CategoryID::AlliedProjectile);
	missileGuider.action = derivedAction<Projectile>([this](Projectile& missile, sf::Time)
	{
		// Ignore unguided bullets
		if (!missile.isGuided())
			return;

		float minDistance = std::numeric_limits<float>::max();
		Aircraft* closestEnemy = nullptr;

		// Find closest enemy
		for (Aircraft* enemy : mActiveEnemies)
		{
			float enemyDistance = distance(missile, *enemy);

			if (enemyDistance < minDistance)
			{
				closestEnemy = enemy;
				minDistance = enemyDistance;
			}
		}

		if (closestEnemy)
			missile.guideTowards(closestEnemy->getWorldPosition());
	});

	// Push commands, reset active enemies
	mCommandQueue.push(enemyCollector);
	mCommandQueue.push(missileGuider);
	mActiveEnemies.clear();
}

sf::FloatRect World::getViewBounds() const
{
	return sf::FloatRect(mCamera.getCenter() - mCamera.getSize() / 2.f, mCamera.getSize());
}

sf::FloatRect World::getBattlefieldBounds() const
{
	// Return view bounds + some area at top, where enemies spawn
	sf::FloatRect bounds = getViewBounds();
	bounds.top -= 100.f;
	bounds.height += 100.f;

	return bounds;
}