#include <GigaLearnCPP/Learner.h>

#include <RLGymCPP/Rewards/CommonRewards.h>
#include <RLGymCPP/Rewards/ZeroSumReward.h>
#include <RLGymCPP/TerminalConditions/NoTouchCondition.h>
#include <RLGymCPP/TerminalConditions/GoalScoreCondition.h>
#include <RLGymCPP/OBSBuilders/DefaultObs.h>
#include <RLGymCPP/OBSBuilders/AdvancedObs.h>
#include <RLGymCPP/StateSetters/KickoffState.h>
#include <RLGymCPP/StateSetters/RandomState.h>
#include <RLGymCPP/ActionParsers/DefaultAction.h>
#include "NigelRewards.h"

using namespace GGL; // GigaLearn
using namespace RLGC; // RLGymCPP

// Create the RLGymCPP environment for each of our games
EnvCreateResult EnvCreateFunc(int index) {
	// Nigel skillbot rewards: prioritizes dribbles, aerials, flip resets, and mechanical play
	std::vector<WeightedReward> rewards = {
		// Movement - slightly boosted for mechanical play
		{ new AirReward(), 0.15f },
		{ new SpeedReward(), 0.3f },
		{ new WavedashReward(), 0.8f },

		// Dribbling & ball carry - core of the skillbot identity
		{ new GroundDribbleReward(), 18.0f },
		{ new AirDribbleReward(300.0f, 300.0f), 25.0f },
		{ new BallCarryReward(), 12.0f },
		{ new AerialPossessionReward(400.0f), 8.0f },

		// Flip resets - big event reward to incentivize this rare mechanic
		{ new FlipResetReward(), 60.0f },

		// Touch quality - favor controlled touches over power shots
		{ new ControlledTouchReward(), 10.0f },
		{ new StrongTouchReward(20, 100), 8 },
		{ new TouchBallReward(), 0.5f },
		{ new TouchAccelReward(), 0.05f },

		// Player-ball approach
		{ new FaceBallReward(), 0.8f },
		{ new VelocityPlayerToBallReward(), 5.f },

		// Ball-goal - still want to score
		{ new ZeroSumReward(new VelocityBallToGoalReward(), 1), 2.0f },

		// Boost - balanced management
		{ new PickupBoostReward(), 12.f },
		{ new SaveBoostReward(), 0.6f },

		// Game events - goal stays high, demos/bumps heavily reduced
		{ new ZeroSumReward(new BumpReward(), 0.5f), 3 },
		{ new ZeroSumReward(new DemoReward(), 0.5f), 5 },
		{ new GoalReward(), 150 },
		{ new SaveReward(), 0.3f },
		{ new ShotReward(), 0.8f },
		{ new AssistReward(), 0.0f },
	};

	std::vector<TerminalCondition*> terminalConditions = {
		new NoTouchCondition(10),
		new GoalScoreCondition()
	};

	// Make the arena
	int playersPerTeam = 1;
	auto arena = Arena::Create(GameMode::SOCCAR);
	for (int i = 0; i < playersPerTeam; i++) {
		arena->AddCar(Team::BLUE);
		arena->AddCar(Team::ORANGE);
	}

	EnvCreateResult result = {};
	result.actionParser = new DefaultAction();
	result.obsBuilder = new AdvancedObs();
	result.stateSetter = new KickoffState();
	result.terminalConditions = terminalConditions;
	result.rewards = rewards;

	result.arena = arena;

	return result;
}

void StepCallback(Learner* learner, const std::vector<GameState>& states, Report& report) {
	// To prevent expensive metrics from eating at performance, we will only run them on 1/4th of steps
	// This doesn't really matter unless you have expensive metrics (which this example doesn't)
	bool doExpensiveMetrics = (rand() % 4) == 0;

	// Add our metrics
	for (auto& state : states) {
		if (doExpensiveMetrics) {
			for (auto& player : state.players) {
				report.AddAvg("Player/In Air Ratio", !player.isOnGround);
				report.AddAvg("Player/Ball Touch Ratio", player.ballTouchedStep);
				report.AddAvg("Player/Demoed Ratio", player.isDemoed);

				report.AddAvg("Player/Speed", player.vel.Length());
				Vec dirToBall = (state.ball.pos - player.pos).Normalized();
				report.AddAvg("Player/Speed Towards Ball", RS_MAX(0, player.vel.Dot(dirToBall)));

				report.AddAvg("Player/Boost", player.boost);

				if (player.ballTouchedStep)
					report.AddAvg("Player/Touch Height", state.ball.pos.z);

				// Nigel skill metrics
				float ballDist = player.pos.Dist(state.ball.pos);
				Vec ballRel = state.ball.pos - player.pos;
				bool ballAbove = (ballRel.z > 60 && ballRel.z < 300);
				float horizDist = Vec(ballRel.x, ballRel.y, 0).Length();

				// Ground dribble: on ground, ball balanced on car
				bool groundDribble = player.isOnGround && ballAbove && horizDist < 250;
				report.AddAvg("Nigel/Ground Dribble Ratio", groundDribble);

				// Air dribble: both in air, ball close
				bool airDribble = !player.isOnGround && player.pos.z > 300
					&& state.ball.pos.z > 300 && ballDist < 300;
				report.AddAvg("Nigel/Air Dribble Ratio", airDribble);

				// Aerial possession: in air with ball nearby
				bool aerialPoss = !player.isOnGround && ballDist < 400;
				report.AddAvg("Nigel/Aerial Possession Ratio", aerialPoss);

				// Flip reset detection
				if (player.prev) {
					bool flipReset = !player.isOnGround
						&& (player.prev->hasDoubleJumped || player.prev->hasFlipped)
						&& !player.hasDoubleJumped && !player.hasFlipped
						&& player.ballTouchedStep;
					report.AddAvg("Nigel/Flip Reset Ratio", flipReset);
				}
			}
		}

		if (state.goalScored)
			report.AddAvg("Game/Goal Speed", state.ball.vel.Length());
	}
}

int main(int argc, char* argv[]) {
	// Initialize RocketSim with collision meshes
	// Change this path to point to your meshes!
	RocketSim::Init("../../../collision_meshes");

	bool renderMode = false;
	for (int i = 1; i < argc; i++) {
		if (std::string(argv[i]) == "--render") {
			renderMode = true;
			break;
		}
	}

	// Make configuration for the learner
	LearnerConfig cfg = {};
	cfg.checkpointFolder = "../../../checkpoints";

	cfg.deviceType = LearnerDeviceType::CPU;

	cfg.tickSkip = 8;
	cfg.actionDelay = cfg.tickSkip - 1; // Normal value in other RLGym frameworks

	// Play around with this to see what the optimal is for your machine, more games will consume more RAM
	cfg.numGames = 256;

	// Leave this empty to use a random seed each run
	// The random seed can have a strong effect on the outcome of a run
	cfg.randomSeed = 123;

	int tsPerItr = 50'000;
	cfg.ppo.tsPerItr = tsPerItr;
	cfg.ppo.batchSize = tsPerItr;
	cfg.ppo.miniBatchSize = 50'000; // Lower this if too much VRAM is being allocated

	// 2 epochs for more stable learning of complex mechanics
	cfg.ppo.epochs = 2;

	// Higher entropy encourages exploration of dribbles/aerials/flip resets
	cfg.ppo.entropyScale = 0.045f;

	// Higher gamma for longer-horizon play (dribble sequences take many steps)
	cfg.ppo.gaeGamma = 0.985;

	// Good learning rate to start
	cfg.ppo.policyLR = 1.5e-4;
	cfg.ppo.criticLR = 1.5e-4;

	cfg.ppo.sharedHead.layerSizes = { 256, 256 };
	cfg.ppo.policy.layerSizes = { 256, 256, 256 };
	cfg.ppo.critic.layerSizes = { 256, 256, 256 };

	auto optim = ModelOptimType::ADAM;
	cfg.ppo.policy.optimType = optim;
	cfg.ppo.critic.optimType = optim;
	cfg.ppo.sharedHead.optimType = optim;

	auto activation = ModelActivationType::RELU;
	cfg.ppo.policy.activationType = activation;
	cfg.ppo.critic.activationType = activation;
	cfg.ppo.sharedHead.activationType = activation;

	bool addLayerNorm = true;
	cfg.ppo.policy.addLayerNorm = addLayerNorm;
	cfg.ppo.critic.addLayerNorm = addLayerNorm;
	cfg.ppo.sharedHead.addLayerNorm = addLayerNorm;

	cfg.sendMetrics = !renderMode; // Send metrics
	cfg.renderMode = renderMode; // Don't render

	// Make the learner with the environment creation function and the config we just made
	Learner* learner = new Learner(EnvCreateFunc, cfg, StepCallback);

	// Start learning!
	learner->Start();

	return EXIT_SUCCESS;
}
