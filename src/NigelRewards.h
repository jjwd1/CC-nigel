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
		// Track last N steer inputs to detect spammy oscillation patterns.
		// Per-player history (2 players per game) so inputs don't mix.
		static constexpr int HISTORY_SIZE = 15;
		static constexpr int MAX_PLAYERS = 2;
		float steerHistory[MAX_PLAYERS][HISTORY_SIZE] = {};
		int historyIndex[MAX_PLAYERS] = {};
		bool historyFull[MAX_PLAYERS] = {};

		virtual void Reset(const GameState& initialState) override {
			for (int p = 0; p < MAX_PLAYERS; p++) {
				for (int i = 0; i < HISTORY_SIZE; i++) steerHistory[p][i] = 0;
				historyIndex[p] = 0;
				historyFull[p] = false;
			}
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Find this player's index in the game
			int pIdx = 0;
			for (int i = 0; i < (int)state.players.size(); i++) {
				if (&state.players[i] == &player) { pIdx = i; break; }
			}
			if (pIdx >= MAX_PLAYERS) pIdx = 0;

			// On ground: track steer. In air: track yaw, but NOT while air rolling
			// — yaw oscillation during tornado spins is legitimate aerial control.
			bool airRolling = !player.isOnGround && fabsf(player.prevAction.roll) > 0.5f;
			float curInput = player.isOnGround ? player.prevAction.steer :
				(airRolling ? 0.0f : player.prevAction.yaw);

			steerHistory[pIdx][historyIndex[pIdx]] = curInput;
			historyIndex[pIdx] = (historyIndex[pIdx] + 1) % HISTORY_SIZE;
			if (historyIndex[pIdx] == 0) historyFull[pIdx] = true;

			int count = historyFull[pIdx] ? HISTORY_SIZE : historyIndex[pIdx];
			if (count < 3)
				return 0;

			float penalty = 0;

			// Count direction changes over the history window.
			// If the bot is spamming left-right, we'll see many sign flips.
			int directionChanges = 0;
			float prevVal = 0;
			bool prevSet = false;

			for (int i = 0; i < count; i++) {
				int idx = historyFull[pIdx] ? (historyIndex[pIdx] + i) % HISTORY_SIZE : i;
				float val = steerHistory[pIdx][idx];

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
		static constexpr int MAX_PLAYERS = 2;
		static constexpr int MAX_FOLLOWUP_TICKS = 15; // ~1 second at 15 steps/sec (tickSkip=8)
		bool hadFlipReset[MAX_PLAYERS] = {};
		int ticksSinceReset[MAX_PLAYERS] = {};

		virtual void Reset(const GameState& initialState) override {
			for (int p = 0; p < MAX_PLAYERS; p++) {
				hadFlipReset[p] = false;
				ticksSinceReset[p] = 0;
			}
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			int pIdx = 0;
			for (int i = 0; i < (int)state.players.size(); i++) {
				if (&state.players[i] == &player) { pIdx = i; break; }
			}
			if (pIdx >= MAX_PLAYERS) pIdx = 0;

			bool gotResetNow = !player.isOnGround && state.ball.pos.z > 300 &&
				(player.prev->hasDoubleJumped || player.prev->hasFlipped) &&
				!player.hasDoubleJumped && !player.hasFlipped &&
				player.ballTouchedStep;

			if (gotResetNow) {
				hadFlipReset[pIdx] = true;
				ticksSinceReset[pIdx] = 0;
			}

			if (hadFlipReset[pIdx]) {
				ticksSinceReset[pIdx]++;

				if (!player.isOnGround && (player.isFlipping || player.hasDoubleJumped || player.hasFlipped)) {
					hadFlipReset[pIdx] = false;
					return 1.0f;
				}

				if (ticksSinceReset[pIdx] > MAX_FOLLOWUP_TICKS || player.isOnGround) {
					hadFlipReset[pIdx] = false;
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
		static constexpr int MAX_PLAYERS = 2;
		static constexpr int CHAIN_WINDOW_TICKS = 60; // ~4 seconds at 15 steps/sec
		int chainCount[MAX_PLAYERS] = {};
		int ticksSinceLastReset[MAX_PLAYERS] = {};
		bool tracking[MAX_PLAYERS] = {};

		virtual void Reset(const GameState& initialState) override {
			for (int p = 0; p < MAX_PLAYERS; p++) {
				chainCount[p] = 0;
				ticksSinceLastReset[p] = 0;
				tracking[p] = false;
			}
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			int pIdx = 0;
			for (int i = 0; i < (int)state.players.size(); i++) {
				if (&state.players[i] == &player) { pIdx = i; break; }
			}
			if (pIdx >= MAX_PLAYERS) pIdx = 0;

			bool gotResetNow = !player.isOnGround && state.ball.pos.z > 300 &&
				(player.prev->hasDoubleJumped || player.prev->hasFlipped) &&
				!player.hasDoubleJumped && !player.hasFlipped &&
				player.ballTouchedStep;

			if (tracking[pIdx]) {
				ticksSinceLastReset[pIdx]++;

				if (player.isOnGround || ticksSinceLastReset[pIdx] > CHAIN_WINDOW_TICKS) {
					chainCount[pIdx] = 0;
					tracking[pIdx] = false;
				}
			}

			if (gotResetNow) {
				if (tracking[pIdx]) {
					chainCount[pIdx]++;
					ticksSinceLastReset[pIdx] = 0;
					return (float)chainCount[pIdx];
				} else {
					chainCount[pIdx] = 1;
					ticksSinceLastReset[pIdx] = 0;
					tracking[pIdx] = true;
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
	// Wall carry: reward for dribbling the ball up the wall.
	// Ball must be balanced on car + on wall + moving upward.
	// Bridges ground dribble → wall → aerial transition.
	// =========================================================================
	class WallCarryReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			// Must be on a wall surface
			if (!player.isOnGround || player.pos.z < 200)
				return 0;

			bool nearSideWall = fabsf(player.pos.x) > 3500;
			bool nearBackWall = fabsf(player.pos.y) > 4600;
			if (!nearSideWall && !nearBackWall)
				return 0;

			// Ball must be balanced on/near the car (same idea as ground dribble)
			Vec ballRel = state.ball.pos - player.pos;
			float ballDist = ballRel.Length();
			if (ballDist > 350)
				return 0;

			// Ball should be "above" the car relative to car's up direction
			float upDot = player.rotMat.up.Dot(ballRel);
			if (upDot < 40 || upDot > 300)
				return 0;

			// Must be moving upward on the wall (carrying ball up, not sitting still)
			if (player.vel.z < 100)
				return 0;

			// Scale by height (higher = closer to aerial transition) and speed
			float heightScore = RS_MIN(1.0f, player.pos.z / 1500.0f);
			float speedScore = RS_MIN(1.0f, player.vel.Length() / 1500.0f);

			return heightScore * 0.5f + speedScore * 0.5f;
		}
	};

	// =========================================================================
	// Wall to air transition: event reward for jumping off a wall toward the ball.
	// This is the critical skill that bridges wall play to aerial play.
	// Detects: was on wall last step → now airborne → ball is nearby and above.
	// =========================================================================
	class WallToAirReward : public Reward {
	public:
		static constexpr int MAX_PLAYERS = 2;

		// Track: was the bot on the wall with the ball nearby?
		bool wasOnWallWithBall[MAX_PLAYERS] = {};

		virtual void Reset(const GameState& initialState) override {
			for (int p = 0; p < MAX_PLAYERS; p++)
				wasOnWallWithBall[p] = false;
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			int pIdx = 0;
			for (int i = 0; i < (int)state.players.size(); i++) {
				if (&state.players[i] == &player) { pIdx = i; break; }
			}
			if (pIdx >= MAX_PLAYERS) pIdx = 0;

			// Step 1: Detect "on wall with ball"
			bool onWall = player.isOnGround && player.pos.z > 200 &&
				(fabsf(player.pos.x) > 3500 || fabsf(player.pos.y) > 4600);
			float ballDistNow = player.pos.Dist(state.ball.pos);

			if (onWall && ballDistNow < 600) {
				wasOnWallWithBall[pIdx] = true;
			}

			// Reset if back on ground and not on wall (drove back down)
			if (player.isOnGround && !onWall) {
				wasOnWallWithBall[pIdx] = false;
			}

			// Step 2: Detect transition to airborne after being on wall with ball
			// Uses the flag instead of checking previous frame — avoids missing
			// the event if isOnGround flickered for a frame during the jump.
			if (player.isOnGround || !wasOnWallWithBall[pIdx])
				return 0;

			// Step 3: Ball must be elevated (bot popped/carried it up)
			if (state.ball.pos.z < 200)
				return 0;

			// Step 4: Moving toward ball (actually going after it)
			Vec dirToBall = (state.ball.pos - player.pos).Normalized();
			float speedToBall = player.vel.Dot(dirToBall);
			if (speedToBall < 0)
				return 0;

			// Full sequence confirmed — clear the flag (one-time event)
			wasOnWallWithBall[pIdx] = false;

			// Don't reward if low boost — no penalty, just no incentive
			if (player.boost < 25)
				return 0;

			// Good play — reward jumping off wall toward ball with enough boost
			float ballDist = player.pos.Dist(state.ball.pos);
			if (ballDist > 800)
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

			float baseReward = velScore * distScore;

			// Must be airborne — no reward for running under aerial balls
			if (player.isOnGround)
				return 0;

			float airBonus = RS_MIN(1.0f, RS_MAX(0.0f, player.vel.z) / 1000.0f);
			return baseReward + airBonus;
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
	// Boost while dribbling: bonus for picking up boost pads while carrying
	// the ball. Teaches the bot to route through pads on the way to plays
	// instead of choosing between boost and ball control.
	// =========================================================================
	class BoostWhileDribblingReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Did boost increase this step? (picked up a pad)
			if (player.boost <= player.prev->boost)
				return 0;

			// Ball must be close (possession)
			float ballDist = player.pos.Dist(state.ball.pos);
			if (ballDist > 300)
				return 0;

			// Bot and ball should be moving in similar directions (carrying, not chasing)
			if (player.vel.Length() > 200 && state.ball.vel.Length() > 200) {
				float alignment = player.vel.Normalized().Dot(state.ball.vel.Normalized());
				if (alignment < 0.3f)
					return 0;
			}

			// Flat reward — the event (boost pickup while possessing ball) is what matters
			return 1.0f;
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
	// Relaxed face ball: like FaceBallReward but with a dead zone.
	// No reward when already roughly facing the ball (within ~20 degrees).
	// Prevents obsessive micro-corrections that cause steering jitter.
	// =========================================================================
	class RelaxedFaceBallReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			Vec dirToBall = (state.ball.pos - player.pos).Normalized();
			float dot = player.rotMat.forward.Dot(dirToBall);

			// Already facing ball within ~20 degrees (dot > 0.94) — no reward needed
			if (dot > 0.94f)
				return 0;

			// Scale: further off-target = more reward for turning toward ball
			// Ranges from 0 (at 20 degrees off) to 1 (facing completely away)
			return (0.94f - dot) / 1.94f;
		}
	};

	// =========================================================================
	// Kickoff reward: rewards flipping toward the ball during kickoff
	// In RL, flipping on kickoff is fundamental — it gets you to the ball
	// faster and with more momentum than just driving.
	// =========================================================================
	class KickoffReward : public Reward {
	public:
		static constexpr int MAX_PLAYERS = 2;
		// ~1.5 seconds after hit at 15 steps/sec (tickSkip=8) ≈ 22 ticks
		static constexpr int CHECK_DELAY_TICKS = 22;
		// 2-second window to flip (~30 ticks)
		static constexpr int FLIP_WINDOW_TICKS = 30;

		bool inApproach[MAX_PLAYERS] = {};
		bool waitingToCheck[MAX_PLAYERS] = {};
		bool flipRewarded[MAX_PLAYERS] = {};
		int ticksSinceHit[MAX_PLAYERS] = {};
		int ticksSinceKickoff[MAX_PLAYERS] = {};

		virtual void Reset(const GameState& initialState) override {
			for (int p = 0; p < MAX_PLAYERS; p++) {
				inApproach[p] = true;
				waitingToCheck[p] = false;
				flipRewarded[p] = false;
				ticksSinceHit[p] = 0;
				ticksSinceKickoff[p] = 0;
			}
		}

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			int pIdx = 0;
			for (int i = 0; i < (int)state.players.size(); i++) {
				if (&state.players[i] == &player) { pIdx = i; break; }
			}
			if (pIdx >= MAX_PLAYERS) pIdx = 0;

			// Phase 1: Approaching the ball — reward flipping and speed
			if (inApproach[pIdx]) {
				// Not a kickoff episode if ball isn't at center
				if (fabsf(state.ball.pos.x) > 50 || fabsf(state.ball.pos.y) > 50) {
					inApproach[pIdx] = false;
					return 0;
				}

				// Ball hit — transition to waiting phase
				if (state.ball.vel.Length() > 100) {
					inApproach[pIdx] = false;
					waitingToCheck[pIdx] = true;
					ticksSinceHit[pIdx] = 0;
					return 0;
				}

				ticksSinceKickoff[pIdx]++;

				float reward = 0;

				// Reward speed toward ball
				Vec dirToBall = (state.ball.pos - player.pos).Normalized();
				float speedToBall = player.vel.Dot(dirToBall);
				reward += RS_MAX(0, speedToBall / CommonValues::CAR_MAX_SPEED);

				// One-time bonus for flipping within the 2-second window.
				if (!flipRewarded[pIdx] && ticksSinceKickoff[pIdx] <= FLIP_WINDOW_TICKS &&
					(player.isFlipping || player.hasFlipped)) {
					flipRewarded[pIdx] = true;
					reward += 1.0f;
				}

				return reward;
			}

			// Phase 2: Wait then check ball position
			if (waitingToCheck[pIdx]) {
				ticksSinceHit[pIdx]++;

				if (isFinal) {
					waitingToCheck[pIdx] = false;
					return 0;
				}

				if (ticksSinceHit[pIdx] >= CHECK_DELAY_TICKS) {
					waitingToCheck[pIdx] = false;

					// Is the ball on the opponent's half?
					float oppGoalY = (player.team == Team::BLUE) ? 5120.0f : -5120.0f;
					bool ballOnOppSide = (oppGoalY > 0) ? (state.ball.pos.y > 0) : (state.ball.pos.y < 0);

					if (ballOnOppSide) {
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
