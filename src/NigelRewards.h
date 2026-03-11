#pragma once
#include <RLGymCPP/Rewards/Reward.h>
#include <RLGymCPP/CommonValues.h>
#include <RLGymCPP/Math.h>

namespace RLGC {

	// =========================================================================
	// Ground dribble: ball balanced on/near car while driving on ground
	// =========================================================================
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

			// In air, had used flip/double jump, now has it back, touched ball
			if (!player.isOnGround &&
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

		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Detect flip reset this step
			bool gotResetNow = !player.isOnGround &&
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
}
