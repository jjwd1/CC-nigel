#pragma once
#include <RLGymCPP/Rewards/Reward.h>
#include <RLGymCPP/CommonValues.h>
#include <RLGymCPP/Math.h>

namespace RLGC {

	// =========================================================================
	// Ground dribble: ball balanced on/near car while driving on ground
	// =========================================================================
	// Helper: check if any opponent is also close to the ball (shared possession).
	// Used to prevent cooperative ball-sharing to farm dribble rewards.
	inline bool OpponentNearBall(const Player& player, const GameState& state, float maxDist = 400.0f) {
		for (auto& p : state.players) {
			if (p.team == player.team)
				continue;
			if (p.pos.Dist(state.ball.pos) < maxDist)
				return true;
		}
		return false;
	}

	class GroundDribbleReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.isOnGround)
				return 0;

			Vec ballRelPos = state.ball.pos - player.pos;
			float horizDist = ballRelPos.Length2D();
			float vertDist = ballRelPos.z;

			// Ball should be above the car (z ~80-300) and close horizontally (<250)
			if (horizDist > 250 || vertDist < 60 || vertDist > 300)
				return 0;

			// No reward if opponent is also on top of the ball (prevents co-farming)
			if (OpponentNearBall(player, state, 300.0f))
				return 0;

			float horizScore = 1.0f - (horizDist / 250.0f);
			float vertScore = 1.0f - RS_MIN(1.0f, fabsf(vertDist - 150) / 150.0f);

			// Bonus for moving forward with ball (not sitting still)
			float speedBonus = RS_MIN(1.0f, player.vel.Length() / 1200.0f);

			// Bonus if ball and car velocity aligned
			float velAlignment = 0;
			if (player.vel.Length() > 100 && state.ball.vel.Length() > 100) {
				velAlignment = player.vel.Normalized().Dot(state.ball.vel.Normalized());
				velAlignment = RS_MAX(0, velAlignment);
			}

			return horizScore * vertScore * (0.3f + 0.3f * speedBonus + 0.4f * velAlignment);
		}
	};

	// =========================================================================
	// Steering smoothness: penalize spammy left-right input oscillation.
	// Tracks a 5-frame history of steer/yaw inputs and counts direction
	// changes. 2+ flips in 5 frames = spam. Works on ground AND air.
	// Also checks physical angular velocity oscillation.
	// =========================================================================
	class SteeringSmoothnessPenalty : public Reward {
	public:
		// Track last N steer inputs to detect spammy oscillation patterns
		// (not just frame-to-frame, but also left-left-right-right patterns)
		static constexpr int HISTORY_SIZE = 15;
		float steerHistory[HISTORY_SIZE] = {};
		int historyIndex = 0;
		bool historyFull = false;

		virtual void Reset(const GameState& initialState) override {
			for (int i = 0; i < HISTORY_SIZE; i++) steerHistory[i] = 0;
			historyIndex = 0;
			historyFull = false;
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// On ground: track steer. In air: track yaw, but NOT while air rolling
			// — yaw oscillation during tornado spins is legitimate aerial control.
			bool airRolling = !player.isOnGround && fabsf(player.prevAction.roll) > 0.5f;
			float curInput = player.isOnGround ? player.prevAction.steer :
				(airRolling ? 0.0f : player.prevAction.yaw);

			steerHistory[historyIndex] = curInput;
			historyIndex = (historyIndex + 1) % HISTORY_SIZE;
			if (historyIndex == 0) historyFull = true;

			int count = historyFull ? HISTORY_SIZE : historyIndex;
			if (count < 3)
				return 0;

			float penalty = 0;

			// Count direction changes over the history window.
			// If the bot is spamming left-right, we'll see many sign flips.
			int directionChanges = 0;
			float prevVal = 0;
			bool prevSet = false;

			for (int i = 0; i < count; i++) {
				// Read in chronological order
				int idx = historyFull ? (historyIndex + i) % HISTORY_SIZE : i;
				float val = steerHistory[idx];

				// Skip near-zero inputs (dead zone)
				if (fabsf(val) < 0.1f)
					continue;

				if (prevSet && ((val > 0 && prevVal < 0) || (val < 0 && prevVal > 0))) {
					directionChanges++;
				}

				prevVal = val;
				prevSet = true;
			}

			// 5+ direction changes in 1 second = spamming
			// Legitimate turns rarely flip direction more than 4 times in a second
			if (directionChanges >= 5)
				penalty -= 0.3f * directionChanges;

			// Also check physical angular velocity oscillation
			float curYawVel = player.angVel.z;
			float prevYawVel = player.prev->angVel.z;

			if (fabsf(curYawVel) > 0.2f && fabsf(prevYawVel) > 0.2f) {
				bool signFlipped = (curYawVel > 0 && prevYawVel < 0) ||
					(curYawVel < 0 && prevYawVel > 0);
				if (signFlipped) {
					float magnitude = RS_MIN(1.0f, (fabsf(curYawVel) + fabsf(prevYawVel)) / 6.0f);
					penalty -= magnitude;
				}
			}

			return penalty;
		}
	};

	// =========================================================================
	// Flick: launch ball off car with a jump/flip while dribbling
	// Detects ball going from "on car" to "flying away fast" after a jump
	// =========================================================================
	class FlickReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev || !state.prev)
				return 0;

			// Was the ball on the car last step?
			Vec prevBallRel = state.prev->ball.pos - player.prev->pos;
			float prevHorizDist = prevBallRel.Length2D();
			float prevVertDist = prevBallRel.z;
			bool ballWasOnCar = player.prev->isOnGround && prevHorizDist < 300 && prevVertDist > 40 && prevVertDist < 350;

			if (!ballWasOnCar)
				return 0;

			// Did the player jump or flip?
			bool jumped = !player.isOnGround && player.prev->isOnGround;
			bool flipping = player.isFlipping;
			if (!jumped && !flipping)
				return 0;

			// Did the ball gain significant upward velocity?
			float ballUpVelGain = state.ball.vel.z - state.prev->ball.vel.z;
			if (ballUpVelGain < 200)
				return 0;

			// Scale by how much velocity the ball gained (better flick = more speed)
			float velGain = (state.ball.vel - state.prev->ball.vel).Length();
			float score = RS_MIN(1.0f, velGain / 1500.0f);

			// Bonus if ball is heading toward opponent goal
			Vec goalDir = (player.team == Team::BLUE) ?
				CommonValues::ORANGE_GOAL_BACK : CommonValues::BLUE_GOAL_BACK;
			Vec ballToGoal = (goalDir - state.ball.pos).Normalized();
			float goalAlignment = RS_MAX(0, ballToGoal.Dot(state.ball.vel.Normalized()));

			return score * (0.6f + 0.4f * goalAlignment);
		}
	};

	// =========================================================================
	// Air dribble: ball close to car while both are airborne
	// =========================================================================
	class AirDribbleReward : public Reward {
	public:
		float maxDist;
		float minHeight;
		AirDribbleReward(float maxDist = 300.0f, float minHeight = 300.0f)
			: maxDist(maxDist), minHeight(minHeight) {}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (player.isOnGround)
				return 0;

			if (player.pos.z < minHeight || state.ball.pos.z < minHeight)
				return 0;

			float dist = player.pos.Dist(state.ball.pos);
			if (dist > maxDist)
				return 0;

			float distScore = 1.0f - (dist / maxDist);

			// Height bonus: higher air dribbles are more impressive
			float heightScore = RS_MIN(1.0f, state.ball.pos.z / CommonValues::CEILING_Z);

			// Ball should be ahead/above the car for proper air dribble orientation
			Vec ballRelative = (state.ball.pos - player.pos).Normalized();
			float upDot = player.rotMat.up.Dot(ballRelative);
			float forwardDot = player.rotMat.forward.Dot(ballRelative);
			float orientScore = RS_MAX(0, upDot * 0.5f + forwardDot * 0.5f);

			return distScore * (0.4f + 0.3f * heightScore + 0.3f * orientScore);
		}
	};

	// =========================================================================
	// Air roll during air dribble: small reward for using air roll input
	// while in an air dribble state. Teaches tornado spins and car control
	// during aerial carries — looks stylish and improves car orientation.
	// =========================================================================
	class AirRollDribbleReward : public Reward {
	public:
		float maxDist;
		float minHeight;
		AirRollDribbleReward(float maxDist = 400.0f, float minHeight = 300.0f)
			: maxDist(maxDist), minHeight(minHeight) {}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (player.isOnGround)
				return 0;

			if (player.pos.z < minHeight || state.ball.pos.z < minHeight)
				return 0;

			float dist = player.pos.Dist(state.ball.pos);
			if (dist > maxDist)
				return 0;

			// Must be actively rolling
			if (fabsf(player.prevAction.roll) < 0.5f)
				return 0;

			// Scale by closeness to ball — only reward roll when maintaining the carry
			float distScore = 1.0f - (dist / maxDist);

			return distScore;
		}
	};

	// =========================================================================
	// Aerial touch: bonus for touching ball while both are high in the air
	// Rewards successive aerial touches more than the first
	// =========================================================================
	class AerialTouchReward : public Reward {
	public:
		float minHeight;
		AerialTouchReward(float minHeight = 300.0f) : minHeight(minHeight) {}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.ballTouchedStep)
				return 0;

			if (player.isOnGround || state.ball.pos.z < minHeight)
				return 0;

			// Scale by height — higher touches are harder and more rewarding
			float heightBonus = RS_MIN(1.0f, state.ball.pos.z / CommonValues::CEILING_Z);

			return 0.5f + 0.5f * heightBonus;
		}
	};

	// =========================================================================
	// Flip reset: regain flip by touching ball with wheels while airborne
	// =========================================================================
	class FlipResetReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// In air, ball in air, had used flip/double jump, now has it back, touched ball
			if (!player.isOnGround && state.ball.pos.z > 300 &&
				(player.prev->hasDoubleJumped || player.prev->hasFlipped) &&
				!player.hasDoubleJumped && !player.hasFlipped &&
				player.ballTouchedStep) {
				return 1.0f;
			}

			return 0;
		}
	};

	// =========================================================================
	// Flip reset follow-up: reward using the regained flip (flipping after reset)
	// This creates the full flip-reset-into-shot sequence
	// =========================================================================
	class FlipResetFollowUpReward : public Reward {
	public:
		// Track whether we recently got a flip reset
		bool hadFlipReset = false;
		int ticksSinceReset = 0;
		static constexpr int MAX_FOLLOWUP_TICKS = 15; // ~1 second at 15 steps/sec (tickSkip=8)

		virtual void Reset(const GameState& initialState) override {
			hadFlipReset = false;
			ticksSinceReset = 0;
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Detect flip reset this step (ball must be in the air)
			bool gotResetNow = !player.isOnGround && state.ball.pos.z > 300 &&
				(player.prev->hasDoubleJumped || player.prev->hasFlipped) &&
				!player.hasDoubleJumped && !player.hasFlipped &&
				player.ballTouchedStep;

			if (gotResetNow) {
				hadFlipReset = true;
				ticksSinceReset = 0;
			}

			if (hadFlipReset) {
				ticksSinceReset++;

				// Used the flip! (flipping or double jumped after reset)
				if (!player.isOnGround && (player.isFlipping || player.hasDoubleJumped || player.hasFlipped)) {
					hadFlipReset = false;
					return 1.0f;
				}

				// Too long since reset, give up
				if (ticksSinceReset > MAX_FOLLOWUP_TICKS || player.isOnGround) {
					hadFlipReset = false;
				}
			}

			return 0;
		}
	};

	// =========================================================================
	// Chained flip resets: escalating reward for getting multiple flip resets
	// in a single aerial play (within ~4 seconds of each other).
	// Double flip reset = big reward, triple = massive reward.
	// Teaches the bot to maintain aerial control after a reset and go for another.
	// =========================================================================
	class ChainedFlipResetReward : public Reward {
	public:
		int chainCount = 0;
		int ticksSinceLastReset = 0;
		bool tracking = false;
		// ~4 seconds at 15 steps/sec (tickSkip=8) = 60 ticks
		static constexpr int CHAIN_WINDOW_TICKS = 60;

		virtual void Reset(const GameState& initialState) override {
			chainCount = 0;
			ticksSinceLastReset = 0;
			tracking = false;
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Detect flip reset this step (ball must be in the air)
			bool gotResetNow = !player.isOnGround && state.ball.pos.z > 300 &&
				(player.prev->hasDoubleJumped || player.prev->hasFlipped) &&
				!player.hasDoubleJumped && !player.hasFlipped &&
				player.ballTouchedStep;

			if (tracking) {
				ticksSinceLastReset++;

				// Chain expired — landed or too much time passed
				if (player.isOnGround || ticksSinceLastReset > CHAIN_WINDOW_TICKS) {
					chainCount = 0;
					tracking = false;
				}
			}

			if (gotResetNow) {
				if (tracking) {
					// This is a chained reset! (2nd, 3rd, etc.)
					chainCount++;
					ticksSinceLastReset = 0;
					// Escalating reward: 2nd reset = 1.0, 3rd = 2.0, 4th = 3.0...
					return (float)chainCount;
				} else {
					// First reset — start tracking the chain
					chainCount = 1;
					ticksSinceLastReset = 0;
					tracking = true;
					// No bonus for the first one (FlipResetReward handles that)
					return 0;
				}
			}

			return 0;
		}
	};

	// =========================================================================
	// Aerial possession: in air with ball nearby (encourages staying close to ball in air)
	// =========================================================================
	class AerialPossessionReward : public Reward {
	public:
		float possessionDist;
		AerialPossessionReward(float possessionDist = 400.0f) : possessionDist(possessionDist) {}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (player.isOnGround)
				return 0;

			float dist = player.pos.Dist(state.ball.pos);
			if (dist > possessionDist)
				return 0;

			return 1.0f - (dist / possessionDist);
		}
	};

	// =========================================================================
	// Controlled touch: gentle touches that keep the ball close (for dribbling)
	// =========================================================================
	class ControlledTouchReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!state.prev || !player.ballTouchedStep)
				return 0;

			float ballSpeedChange = fabsf(state.ball.vel.Length() - state.prev->ball.vel.Length());
			float maxChange = 2000.0f;

			// Gentle touch = high reward, hard smash = low reward
			return 1.0f - RS_MIN(1.0f, ballSpeedChange / maxChange);
		}
	};

	// =========================================================================
	// Ball carry: ball close above car at any height (ground or air carry)
	// =========================================================================
	class BallCarryReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			Vec ballRelPos = state.ball.pos - player.pos;

			// Ball must be above the car
			float upDot = player.rotMat.up.Dot(ballRelPos);
			if (upDot < 50 || upDot > 350)
				return 0;

			// No reward if opponent is also on top of the ball
			if (OpponentNearBall(player, state, 300.0f))
				return 0;

			// Ball must be close horizontally relative to car orientation
			float rightDot = fabsf(player.rotMat.right.Dot(ballRelPos));
			float forwardDot = fabsf(player.rotMat.forward.Dot(ballRelPos));

			if (rightDot > 150 || forwardDot > 200)
				return 0;

			float closenessScore = 1.0f - RS_MIN(1.0f, player.pos.Dist(state.ball.pos) / 400.0f);
			return closenessScore;
		}
	};

	// =========================================================================
	// Dribble toward goal: rewards moving ball toward opponent goal while carrying
	// This connects dribbles to actual scoring opportunities
	// =========================================================================
	class DribbleToGoalReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			Vec ballRelPos = state.ball.pos - player.pos;
			float dist = ballRelPos.Length();
			if (dist > 400)
				return 0;

			// Ball must be above the car (actually carrying, not just chasing)
			if (ballRelPos.z < 40 || ballRelPos.z > 350)
				return 0;

			// No reward if opponent is also near the ball
			if (OpponentNearBall(player, state, 300.0f))
				return 0;

			// Must be moving
			if (player.vel.Length() < 200)
				return 0;

			Vec goalPos = (player.team == Team::BLUE) ?
				CommonValues::ORANGE_GOAL_BACK : CommonValues::BLUE_GOAL_BACK;
			Vec toGoal = (goalPos - player.pos).Normalized();
			float goalward = player.vel.Normalized().Dot(toGoal);

			if (goalward < 0)
				return 0;

			// Scale by closeness to ball (tighter carry = more reward)
			float closeness = 1.0f - RS_MIN(1.0f, dist / 400.0f);

			return goalward * closeness;
		}
	};

	// =========================================================================
	// Ball height reward: rewards ball being high in the air near the car
	// Encourages popping ball up for aerial play setups
	// =========================================================================
	class BallHeightNearCarReward : public Reward {
	public:
		float maxDist;
		BallHeightNearCarReward(float maxDist = 600.0f) : maxDist(maxDist) {}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			float dist = player.pos.Dist(state.ball.pos);
			if (dist > maxDist)
				return 0;

			if (state.ball.pos.z < 200)
				return 0;

			float heightScore = RS_MIN(1.0f, state.ball.pos.z / CommonValues::CEILING_Z);
			float distScore = 1.0f - (dist / maxDist);

			return heightScore * distScore;
		}
	};

	// =========================================================================
	// Wall play: continuous reward for being on the wall near the ball.
	// Encourages the bot to actually drive up walls instead of staying on ground.
	// Uses position-based wall detection (isOnGround + high up + near wall edge).
	// =========================================================================
	class WallPlayReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			// Detect "on wall": wheels on surface, elevated, near a wall edge
			// Side walls at x=±4096, back walls at y=±5120
			if (!player.isOnGround || player.pos.z < 200)
				return 0;

			bool nearSideWall = fabsf(player.pos.x) > 3500;
			bool nearBackWall = fabsf(player.pos.y) > 4600;
			if (!nearSideWall && !nearBackWall)
				return 0;

			// Must be near the ball for this to be useful
			float ballDist = player.pos.Dist(state.ball.pos);
			if (ballDist > 1000)
				return 0;

			// Scale by height (higher on wall = better) and closeness to ball
			float heightScore = RS_MIN(1.0f, player.pos.z / 1500.0f);
			float distScore = 1.0f - (ballDist / 1000.0f);

			return heightScore * distScore;
		}
	};

	// =========================================================================
	// Wall to air transition: event reward for jumping off a wall toward the ball.
	// This is the critical skill that bridges wall play to aerial play.
	// Detects: was on wall last step → now airborne → ball is nearby and above.
	// =========================================================================
	class WallToAirReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Was on wall last step?
			bool prevOnWall = player.prev->isOnGround && player.prev->pos.z > 200 &&
				(fabsf(player.prev->pos.x) > 3500 || fabsf(player.prev->pos.y) > 4600);

			if (!prevOnWall)
				return 0;

			// Now airborne?
			if (player.isOnGround)
				return 0;

			// Need boost to actually reach the ball after leaving the wall
			if (player.boost < 15)
				return 0;

			// Ball should be nearby and at a decent height
			float ballDist = player.pos.Dist(state.ball.pos);
			if (ballDist > 800 || state.ball.pos.z < 200)
				return 0;

			// Moving toward ball?
			Vec dirToBall = (state.ball.pos - player.pos).Normalized();
			float speedToBall = player.vel.Dot(dirToBall);
			if (speedToBall < 0)
				return 0;

			float distScore = 1.0f - (ballDist / 800.0f);
			float velScore = RS_MIN(1.0f, speedToBall / 1500.0f);

			return distScore * (0.5f + 0.5f * velScore);
		}
	};

	// =========================================================================
	// Flick when pressured: big bonus for flicking when opponent is close.
	// Teaches the bot to flick over diving opponents or toward goal under pressure.
	// =========================================================================
	class FlickWhenPressuredReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev || !state.prev)
				return 0;

			// Was the ball on the car last step?
			Vec prevBallRel = state.prev->ball.pos - player.prev->pos;
			bool ballWasOnCar = player.prev->isOnGround && prevBallRel.Length2D() < 300
				&& prevBallRel.z > 40 && prevBallRel.z < 350;
			if (!ballWasOnCar)
				return 0;

			// Did the player jump or flip?
			bool jumped = !player.isOnGround && player.prev->isOnGround;
			if (!jumped && !player.isFlipping)
				return 0;

			// Did the ball gain upward velocity? (actual flick)
			float ballUpVelGain = state.ball.vel.z - state.prev->ball.vel.z;
			if (ballUpVelGain < 200)
				return 0;

			// Find closest opponent
			float closestOppDist = 99999;
			float oppSpeedToward = 0;
			for (auto& p : state.players) {
				if (p.team == player.team)
					continue;
				float d = p.pos.Dist(player.pos);
				if (d < closestOppDist) {
					closestOppDist = d;
					Vec dirToMe = (player.pos - p.pos).Normalized();
					oppSpeedToward = p.vel.Dot(dirToMe);
				}
			}

			float reward = 0;

			// Opponent close (< 1500) and rushing in — flick is a great choice
			if (closestOppDist < 1500 && oppSpeedToward > 200) {
				float pressureScore = (1.0f - closestOppDist / 1500.0f);
				float rushScore = RS_MIN(1.0f, oppSpeedToward / 1500.0f);
				reward += pressureScore * rushScore;
			}

			// Bonus: ball heading toward opponent goal after flick
			Vec goalDir = (player.team == Team::BLUE) ?
				CommonValues::ORANGE_GOAL_BACK : CommonValues::BLUE_GOAL_BACK;
			Vec ballToGoal = (goalDir - state.ball.pos).Normalized();
			float goalAlignment = RS_MAX(0, ballToGoal.Dot(state.ball.vel.Normalized()));
			reward += goalAlignment * 0.5f;

			return reward;
		}
	};

	// =========================================================================
	// Go for aerial: rewards moving upward toward a ball that's high in the air.
	// Teaches the bot to actually jump/boost toward loose aerial balls instead
	// of watching them float overhead. Continuous reward that scales with
	// how well the bot is closing distance to an elevated ball.
	// =========================================================================
	class GoForAerialReward : public Reward {
	public:
		float minBallHeight;
		GoForAerialReward(float minBallHeight = 400.0f) : minBallHeight(minBallHeight) {}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			// Ball must be meaningfully in the air
			if (state.ball.pos.z < minBallHeight)
				return 0;

			// Don't reward if already covered by AirDribbleReward (ball on car in air)
			float ballDist = player.pos.Dist(state.ball.pos);
			if (ballDist < 200 && !player.isOnGround)
				return 0;

			// Ball within reachable range
			if (ballDist > 2500)
				return 0;

			// Must be moving toward ball (gate at 0 so horizontal movement under
			// an overhead ball still counts — the 3D direction is mostly vertical)
			Vec dirToBall = (state.ball.pos - player.pos).Normalized();
			float speedToBall = player.vel.Dot(dirToBall);
			if (speedToBall < 0)
				return 0;

			float velScore = RS_MIN(1.0f, speedToBall / 1500.0f);
			float distScore = 1.0f - (ballDist / 2500.0f);

			// Bonus for actually being airborne and going up toward it
			float airBonus = 0;
			if (!player.isOnGround && player.vel.z > 100) {
				airBonus = RS_MIN(1.0f, player.vel.z / 1000.0f) * 0.5f;
			}

			return velScore * distScore + airBonus;
		}
	};

	// =========================================================================
	// Low boost aerial penalty: penalize committing to wall/aerial play with
	// very low boost. Teaches the bot to grab boost before going airborne.
	// =========================================================================
	class LowBoostAerialPenalty : public Reward {
	public:
		float boostThreshold;
		LowBoostAerialPenalty(float boostThreshold = 40.0f) : boostThreshold(boostThreshold) {}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (player.boost > boostThreshold)
				return 0;

			// Detect on-wall: isOnGround + elevated + near wall edge
			bool onWall = player.isOnGround && player.pos.z > 200 &&
				(fabsf(player.pos.x) > 3500 || fabsf(player.pos.y) > 4600);

			// Penalize being airborne and elevated
			bool airborne = !player.isOnGround && player.pos.z > 200;

			if (!onWall && !airborne)
				return 0;

			// Scale penalty by height — higher up with no boost is worse
			float heightPenalty = RS_MIN(1.0f, player.pos.z / 1000.0f);

			// Slight forgiveness if very close to ball (committed to a touch)
			float ballDist = player.pos.Dist(state.ball.pos);
			if (ballDist < 200)
				return -0.2f * heightPenalty;

			// Lighter penalty on wall (still has time to drop down and get boost)
			if (onWall)
				return -0.5f * heightPenalty;

			return -1.0f * heightPenalty;
		}
	};

	// =========================================================================
	// Seek boost: reward moving toward the nearest available boost pad when
	// boost is low. Teaches the bot to grab boost on the way to plays instead
	// of driving right past pads.
	// =========================================================================
	class SeekBoostReward : public Reward {
	public:
		float boostThreshold;
		SeekBoostReward(float boostThreshold = 50.0f) : boostThreshold(boostThreshold) {}

		// Big boost pads have z=73, small pads z=70
		static bool IsBigPad(int index) {
			return CommonValues::BOOST_LOCATIONS[index].z > 71;
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			// Only active when boost is low
			if (player.boost > boostThreshold)
				return 0;

			// Only on ground (don't distract from aerial play)
			if (!player.isOnGround)
				return 0;

			// Find nearest available pad, preferring big pads.
			// Big pads get their distance halved so the bot will go for a big pad
			// even if a small pad is somewhat closer.
			const auto& pads = state.boostPads;
			float bestScore = 99999;
			Vec bestPadPos = {};
			bool bestIsBig = false;
			bool found = false;

			for (int i = 0; i < CommonValues::BOOST_LOCATIONS_AMOUNT; i++) {
				if (!pads[i])
					continue;

				Vec padPos = CommonValues::BOOST_LOCATIONS[i];
				float dist = player.pos.Dist(padPos);
				bool big = IsBigPad(i);

				// Big pads appear "closer" — bot will detour for them
				float effectiveDist = big ? dist * 0.4f : dist;

				if (effectiveDist < bestScore) {
					bestScore = effectiveDist;
					bestPadPos = padPos;
					bestIsBig = big;
					found = true;
				}
			}

			if (!found)
				return 0;

			float realDist = player.pos.Dist(bestPadPos);
			if (realDist > 2500)
				return 0;

			// Reward moving toward the chosen pad
			Vec dirToPad = (bestPadPos - player.pos).Normalized();
			float speedToPad = player.vel.Dot(dirToPad);
			if (speedToPad < 0)
				return 0;

			float velScore = RS_MIN(1.0f, speedToPad / 1500.0f);
			float distScore = 1.0f - RS_MIN(1.0f, realDist / 2500.0f);

			// Scale by how low boost is — lower boost = stronger incentive
			float urgency = 1.0f - (player.boost / boostThreshold);

			// Big pad bonus — extra multiplier for going to a big pad
			float bigBonus = bestIsBig ? 1.5f : 1.0f;

			return velScore * distScore * urgency * bigBonus;
		}
	};

	// =========================================================================
	// Waste boost penalty: penalize holding boost input when boost is empty.
	// Teaches the bot to stop pressing boost when it has none.
	// =========================================================================
	class WasteBoostPenalty : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Is the bot pressing boost with no boost left?
			if (player.boost < 1.0f && player.prevAction.boost > 0.5f)
				return -1.0f;

			return 0;
		}
	};

	// =========================================================================
	// Kickoff reward: rewards flipping toward the ball during kickoff
	// In RL, flipping on kickoff is fundamental — it gets you to the ball
	// faster and with more momentum than just driving.
	// =========================================================================
	class KickoffReward : public Reward {
	public:
		bool inApproach = false;    // Ball hasn't been hit yet
		bool waitingToCheck = false; // Ball hit, counting down to check
		bool flipRewarded = false;  // Already gave the one-time flip reward
		int ticksSinceHit = 0;
		int ticksSinceKickoff = 0;
		// ~1.5 seconds after hit at 15 steps/sec (tickSkip=8) ≈ 22 ticks
		static constexpr int CHECK_DELAY_TICKS = 22;
		// 2-second window to flip (~30 ticks)
		static constexpr int FLIP_WINDOW_TICKS = 30;

		virtual void Reset(const GameState& initialState) override {
			inApproach = true;
			waitingToCheck = false;
			flipRewarded = false;
			ticksSinceHit = 0;
			ticksSinceKickoff = 0;
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			// Phase 1: Approaching the ball — reward flipping and speed
			if (inApproach) {
				// Not a kickoff episode if ball isn't at center
				if (fabsf(state.ball.pos.x) > 50 || fabsf(state.ball.pos.y) > 50) {
					inApproach = false;
					return 0;
				}

				// Ball hit — transition to waiting phase
				if (state.ball.vel.Length() > 100) {
					inApproach = false;
					waitingToCheck = true;
					ticksSinceHit = 0;
					return 0;
				}

				ticksSinceKickoff++;

				float reward = 0;

				// Reward speed toward ball
				Vec dirToBall = (state.ball.pos - player.pos).Normalized();
				float speedToBall = player.vel.Dot(dirToBall);
				reward += RS_MAX(0, speedToBall / CommonValues::CAR_MAX_SPEED);

				// One-time bonus for flipping within the 2-second window.
				// Not continuous — so there's no incentive to flip instantly.
				if (!flipRewarded && ticksSinceKickoff <= FLIP_WINDOW_TICKS &&
					(player.isFlipping || player.hasFlipped)) {
					flipRewarded = true;
					reward += 1.0f;
				}

				return reward;
			}

			// Phase 2: Wait 4 seconds then check ball position
			if (waitingToCheck) {
				ticksSinceHit++;

				// Episode ended before we could check (goal scored)
				if (isFinal) {
					waitingToCheck = false;
					return 0;
				}

				if (ticksSinceHit >= CHECK_DELAY_TICKS) {
					waitingToCheck = false;

					// Is the ball on the opponent's half?
					float oppGoalY = (player.team == Team::BLUE) ? 5120.0f : -5120.0f;
					bool ballOnOppSide = (oppGoalY > 0) ? (state.ball.pos.y > 0) : (state.ball.pos.y < 0);

					if (ballOnOppSide) {
						// Scale by how deep into opponent half
						float depth = fabsf(state.ball.pos.y) / 5120.0f;
						return 0.5f + 0.5f * depth;
					}

					return 0;
				}
			}

			return 0;
		}
	};
}
