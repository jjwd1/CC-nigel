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
	// Aerial rewards boosted relative to ground to encourage air play.
	std::vector<WeightedReward> rewards = {

		// --- Core mechanics: dribbling & ball carry ---
		{ new GroundDribbleReward(), 0.75f },           // Keep ball balanced on car
		{ new BallCarryReward(), 1.0f },                // Ball above car (ground or air)
		{ new DribbleToGoalReward(), 1.5f },            // Carry ball toward opponent goal
		{ new FlickReward(), 50.0f },                   // Launch ball off car with flip (event)
		{ new FlickWhenPressuredReward(), 40.0f },      // Flick when opponent diving in / toward goal

		// --- Wall play (bridge from ground to aerial) ---
		{ new WallPlayReward(), 2.0f },                  // On wall near ball (continuous, NEW)
		{ new WallToAirReward(), 15.0f },                // Jump off wall toward ball (event, NEW)

		// --- Aerial mechanics ---
		// Boosted to incentivize aerial play over ground dribbling.
		{ new GoForAerialReward(400.0f), 3.0f },       // Move toward loose balls in the air
		{ new AirDribbleReward(400.0f, 150.0f), 5.0f }, // Carry ball in air
		{ new AerialTouchReward(200.0f), 20.0f },       // Touch ball while high (was 15, lowered minHeight 250->200)
		{ new AerialPossessionReward(500.0f), 1.5f },    // Stay near ball in air (was 0.75)
		{ new BallHeightNearCarReward(800.0f), 1.5f },   // Ball high with car nearby (was 0.5, wider range)

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
		{ new AirReward(), 0.3f },                      // Reward for being airborne (was 0.08)
		{ new WavedashReward(), 0.5f },                 // Wavedash detection (event)
		{ new SteeringSmoothnessPenalty(), 0.3f },      // Penalize steering jitter

		// --- Kickoff ---
		{ new KickoffReward(), 3.0f },                  // Flip toward ball on kickoff (NEW)

		// --- Approach & orientation ---
		{ new FaceBallReward(), 0.15f },                // Face toward ball (continuous)
		{ new VelocityPlayerToBallReward(), 1.0f },     // Move toward ball (continuous)

		// --- Ball toward goal (zero-sum so opponent is penalized) ---
		{ new ZeroSumReward(new VelocityBallToGoalReward(), 1), 2.0f },

		// --- Boost management ---
		{ new PickupBoostReward(), 5.0f },              // Collect boost pads (event, was 3)
		{ new SaveBoostReward(), 0.3f },                // Don't waste all boost (continuous)
		{ new LowBoostAerialPenalty(30.0f), 2.0f },    // Penalize going aerial with low boost

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
	// Full mechanical progression: ground → wall → aerial → air dribble
	//   15% kickoff (normal gameplay + kickoff flip practice)
	//   10% ball on car (ground dribble/flick)
	//   15% wall ball (drive up walls, wall-to-air)
	//   15% air dribble setup (already airborne near ball)
	//   15% loose aerial ball (ball floating in air, car on ground — GO UP)
	//   15% ball rolling to car (catch & carry practice)
	//   15% random (general adaptation)
	auto stateSetter = new CombinedState({
		{ new KickoffState(), 15.0f },
		{ new BallOnCarState(), 10.0f },
		{ new WallBallState(), 15.0f },
		{ new AirDribbleSetup(), 15.0f },
		{ new LooseAerialBallState(), 15.0f },
		{ new BallRollingToCarState(), 15.0f },
		{ new RandomState(true, true, true), 15.0f },
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

				if (player.ballTouchedStep) {
					report.AddAvg("Player/Touch Height", state.ball.pos.z);
					report.AddAvg("Player/Touch Ball Speed", state.ball.vel.Length());
				}

				// Nigel skill metrics
				float ballDist = player.pos.Dist(state.ball.pos);
				Vec ballRel = state.ball.pos - player.pos;
				bool ballAbove = (ballRel.z > 60 && ballRel.z < 300);
				float horizDist = ballRel.Length2D();

				// Near ball: within 500 units (engagement proxy)
				report.AddAvg("Nigel/Near Ball Ratio", ballDist < 500);

				// Ground dribble detection
				bool groundDribble = player.isOnGround && ballAbove && horizDist < 250;
				report.AddAvg("Nigel/Ground Dribble Ratio", groundDribble);

				// Dribble toward goal: dribbling while moving toward opponent goal
				if (groundDribble) {
					float goalY = (player.team == Team::BLUE) ? 5120.0f : -5120.0f;
					bool movingToGoal = (goalY > 0) ? (player.vel.y > 200) : (player.vel.y < -200);
					report.AddAvg("Nigel/Dribble To Goal Ratio", movingToGoal);
				}

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

				// Wall play detection
				bool onWall = player.isOnGround && player.pos.z > 200 &&
					(fabsf(player.pos.x) > 3500 || fabsf(player.pos.y) > 4600);
				report.AddAvg("Nigel/On Wall Ratio", onWall);

				// Wall to air transition
				if (player.prev) {
					bool prevOnWall = player.prev->isOnGround && player.prev->pos.z > 200 &&
						(fabsf(player.prev->pos.x) > 3500 || fabsf(player.prev->pos.y) > 4600);
					bool wallToAir = prevOnWall && !player.isOnGround;
					report.AddAvg("Nigel/Wall To Air Ratio", wallToAir);
				}
			}
		}

		if (state.goalScored) {
			report.AddAvg("Game/Goal Speed", state.ball.vel.Length());
			report.AddAvg("Game/Goal Scored", 1.0f);
		} else {
			report.AddAvg("Game/Goal Scored", 0.0f);
		}
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

	// Start learning
	learner->Start();

	return EXIT_SUCCESS;
}
