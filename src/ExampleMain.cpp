#include <GigaLearnCPP/Learner.h>

#include <RLGymCPP/Rewards/CommonRewards.h>
#include <RLGymCPP/Rewards/ZeroSumReward.h>
#include <RLGymCPP/TerminalConditions/NoTouchCondition.h>
#include <RLGymCPP/TerminalConditions/GoalScoreCondition.h>
#include <RLGymCPP/OBSBuilders/DefaultObs.h>
#include <RLGymCPP/OBSBuilders/AdvancedObs.h>
#include <RLGymCPP/StateSetters/KickoffState.h>
#include <RLGymCPP/StateSetters/RandomState.h>
#include <RLGymCPP/StateSetters/CombinedState.h>
#include <RLGymCPP/ActionParsers/DefaultAction.h>
#include "NigelRewards.h"
#include "NigelStateSetters.h"

using namespace GGL; // GigaLearn
using namespace RLGC; // RLGymCPP

// Create the RLGymCPP environment for each of our games
EnvCreateResult EnvCreateFunc(int index) {

	// === REWARDS ===
	// Designed to produce a mechanical skillbot:
	//   ground dribbles -> flicks -> air dribbles -> flip resets -> goals
	std::vector<WeightedReward> rewards = {

		// --- Core mechanics: dribbling & ball carry ---
		// IMPORTANT: These are continuous (per-step) rewards. Keep weights LOW
		// so accumulated dribble reward doesn't dwarf the one-time goal reward.
		// At tickSkip=8, ~15 steps/sec. A 3-second dribble should earn ~100-150,
		// comparable to but less than a goal (300).
		{ new GroundDribbleReward(), 3.0f },            // Keep ball balanced on car
		{ new BallCarryReward(), 2.0f },                // Ball above car (ground or air)
		{ new DribbleToGoalReward(), 4.0f },            // Carry ball toward opponent goal
		{ new FlickReward(), 50.0f },                   // Launch ball off car with flip (event)

		// --- Aerial mechanics ---
		// Also continuous — keep low to avoid "hover near ball forever" loop
		{ new AirDribbleReward(350.0f, 250.0f), 4.0f },    // Carry ball in air
		{ new AerialTouchReward(250.0f), 15.0f },           // Touch ball while high (event)
		{ new AerialPossessionReward(500.0f), 1.5f },       // Stay near ball in air
		{ new BallHeightNearCarReward(600.0f), 1.0f },      // Ball high with car nearby

		// --- Flip resets ---
		{ new FlipResetReward(), 60.0f },               // Get a flip reset (rare, big event)
		{ new FlipResetFollowUpReward(), 40.0f },       // Use the regained flip after reset

		// --- Touch quality ---
		{ new ControlledTouchReward(), 5.0f },          // Gentle touches for dribble control
		{ new StrongTouchReward(20, 100), 5.0f },       // Powerful hits when shooting
		{ new TouchBallReward(), 1.0f },                // Any ball touch (baseline)
		{ new TouchAccelReward(), 0.05f },              // Speed up ball on touch

		// --- Movement fundamentals ---
		{ new SpeedReward(), 0.15f },                   // Keep moving (continuous, keep tiny)
		{ new AirReward(), 0.08f },                     // Small reward for being airborne
		{ new WavedashReward(), 0.5f },                 // Wavedash detection (event)

		// --- Approach & orientation ---
		{ new FaceBallReward(), 0.3f },                 // Face toward ball (continuous)
		{ new VelocityPlayerToBallReward(), 2.0f },     // Move toward ball (continuous)

		// --- Ball toward goal (zero-sum so opponent is penalized) ---
		{ new ZeroSumReward(new VelocityBallToGoalReward(), 1), 4.0f },

		// --- Boost management ---
		{ new PickupBoostReward(), 6.0f },              // Collect boost pads (event)
		{ new SaveBoostReward(), 0.3f },                // Don't waste all boost (continuous)

		// --- Game events - these must dominate to prevent loops ---
		{ new GoalReward(), 300.0f },                   // Score goals (THE objective)
		{ new ZeroSumReward(new BumpReward(), 0.5f), 2.0f },
		{ new ZeroSumReward(new DemoReward(), 0.5f), 3.0f },
		{ new SaveReward(), 0.3f },
		{ new ShotReward(), 2.0f },
	};

	// === TERMINAL CONDITIONS ===
	std::vector<TerminalCondition*> terminalConditions = {
		new NoTouchCondition(15),   // 15s without touch (longer to allow aerial attempts)
		new GoalScoreCondition()
	};

	// === STATE SETTERS ===
	// Mix of scenarios so the bot gets varied practice:
	//   40% kickoff (normal gameplay)
	//   25% ball on car (ground dribble/flick practice)
	//   15% air dribble setup (aerial practice)
	//   10% ball rolling to car (catch & carry practice)
	//   10% random (general adaptation)
	auto stateSetter = new CombinedState({
		{ new KickoffState(), 40.0f },
		{ new BallOnCarState(), 25.0f },
		{ new AirDribbleSetup(), 15.0f },
		{ new BallRollingToCarState(), 10.0f },
		{ new RandomState(true, true, true), 10.0f },
	});

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
	result.stateSetter = stateSetter;
	result.terminalConditions = terminalConditions;
	result.rewards = rewards;

	result.arena = arena;

	return result;
}

void StepCallback(Learner* learner, const std::vector<GameState>& states, Report& report) {
	bool doExpensiveMetrics = (rand() % 4) == 0;

	for (auto& state : states) {
		if (doExpensiveMetrics) {
			for (auto& player : state.players) {
				report.AddAvg("Player/In Air Ratio", !player.isOnGround);
				report.AddAvg("Player/Ball Touch Ratio", player.ballTouchedStep);
				report.AddAvg("Player/Demoed Ratio", player.isDemoed);
				report.AddAvg("Player/Speed", player.vel.Length());
				report.AddAvg("Player/Boost", player.boost);

				Vec dirToBall = (state.ball.pos - player.pos).Normalized();
				report.AddAvg("Player/Speed Towards Ball", RS_MAX(0, player.vel.Dot(dirToBall)));

				if (player.ballTouchedStep)
					report.AddAvg("Player/Touch Height", state.ball.pos.z);

				// Nigel skill metrics
				float ballDist = player.pos.Dist(state.ball.pos);
				Vec ballRel = state.ball.pos - player.pos;
				bool ballAbove = (ballRel.z > 60 && ballRel.z < 300);
				float horizDist = ballRel.Length2D();

				// Ground dribble detection
				bool groundDribble = player.isOnGround && ballAbove && horizDist < 250;
				report.AddAvg("Nigel/Ground Dribble Ratio", groundDribble);

				// Air dribble detection
				bool airDribble = !player.isOnGround && player.pos.z > 300
					&& state.ball.pos.z > 300 && ballDist < 350;
				report.AddAvg("Nigel/Air Dribble Ratio", airDribble);

				// Aerial possession
				bool aerialPoss = !player.isOnGround && ballDist < 500;
				report.AddAvg("Nigel/Aerial Possession Ratio", aerialPoss);

				// Flip reset detection
				if (player.prev) {
					bool flipReset = !player.isOnGround
						&& (player.prev->hasDoubleJumped || player.prev->hasFlipped)
						&& !player.hasDoubleJumped && !player.hasFlipped
						&& player.ballTouchedStep;
					report.AddAvg("Nigel/Flip Reset Ratio", flipReset);
				}

				// Flick detection (ball was on car, now launched)
				if (player.prev && state.prev) {
					Vec prevBallRel = state.prev->ball.pos - player.prev->pos;
					bool wasOnCar = player.prev->isOnGround && prevBallRel.Length2D() < 300
						&& prevBallRel.z > 40 && prevBallRel.z < 350;
					bool launched = !player.isOnGround && (state.ball.vel.z - state.prev->ball.vel.z) > 300;
					report.AddAvg("Nigel/Flick Ratio", wasOnCar && launched);
				}
			}
		}

		if (state.goalScored)
			report.AddAvg("Game/Goal Speed", state.ball.vel.Length());
	}
}

int main(int argc, char* argv[]) {
	// Initialize RocketSim with collision meshes
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

	cfg.deviceType = LearnerDeviceType::GPU_CUDA;

	cfg.tickSkip = 8;
	cfg.actionDelay = cfg.tickSkip - 1;

	cfg.numGames = 256;

	cfg.randomSeed = 123;

	int tsPerItr = 50'000;
	cfg.ppo.tsPerItr = tsPerItr;
	cfg.ppo.batchSize = tsPerItr;
	cfg.ppo.miniBatchSize = 50'000;

	// 3 epochs — mechanical skills need more gradient steps per batch
	cfg.ppo.epochs = 3;

	// Higher entropy for exploration of rare mechanics (flip resets, aerials)
	cfg.ppo.entropyScale = 0.05f;

	// High gamma for long-horizon sequences (dribble -> flick -> goal)
	cfg.ppo.gaeGamma = 0.99;

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

	cfg.sendMetrics = !renderMode;
	cfg.renderMode = renderMode;

	// Make the learner
	Learner* learner = new Learner(EnvCreateFunc, cfg, StepCallback);

	// Start learning!
	learner->Start();

	return EXIT_SUCCESS;
}
