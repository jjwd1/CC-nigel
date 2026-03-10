#pragma once
#include <RLGymCPP/Rewards/Reward.h>
#include <RLGymCPP/CommonValues.h>
#include <RLGymCPP/Math.h>

namespace RLGC {

	// Rewards ground dribbling: ball balanced on/near car while driving on ground
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

			// Bonus if ball and car moving in similar direction
			float velAlignment = 0;
			if (player.vel.Length() > 100 && state.ball.vel.Length() > 100) {
				velAlignment = player.vel.Normalized().Dot(state.ball.vel.Normalized());
				velAlignment = RS_MAX(0, velAlignment);
			}

			return horizScore * vertScore * (0.5f + 0.5f * velAlignment);
		}
	};

	// Rewards air dribbling: ball close to car while both are airborne
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

	// Rewards flip resets: bot regains flip by touching ball with wheels while airborne
	class FlipResetReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!player.prev)
				return 0;

			// Detect flip reset: in air, had used flip/double jump, now has it back, touched ball
			if (!player.isOnGround &&
				(player.prev->hasDoubleJumped || player.prev->hasFlipped) &&
				!player.hasDoubleJumped && !player.hasFlipped &&
				player.ballTouchedStep) {
				return 1.0f;
			}

			return 0;
		}
	};

	// Rewards being in the air with ball possession (close proximity)
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

	// Rewards controlled/soft touches that keep the ball close rather than smashing it away
	class ControlledTouchReward : public Reward {
	public:
		virtual float GetReward(const Player& player, const GameState& state, bool isFinal) override {
			if (!state.prev || !player.ballTouchedStep)
				return 0;

			float ballSpeedChange = fabsf(state.ball.vel.Length() - state.prev->ball.vel.Length());
			float maxChange = 2000.0f;

			// Gentle touch = high reward, hard smash = low reward
			float controlScore = 1.0f - RS_MIN(1.0f, ballSpeedChange / maxChange);

			return controlScore;
		}
	};

	// Rewards carrying the ball: ball close above car at any height (works for both ground and air)
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
}
