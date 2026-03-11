#pragma once
#include <RLGymCPP/StateSetters/StateSetter.h>
#include <RLGymCPP/CommonValues.h>
#include <RLGymCPP/Math.h>
#include <vector>

namespace RLGC {

	using RocketSim::Math::RandFloat;
	using RLGC::Math::RandVec;

	// Helper: collect arena cars into a vector for indexed access
	inline std::vector<Car*> GetCarsVec(Arena* arena) {
		std::vector<Car*> v(arena->_cars.begin(), arena->_cars.end());
		return v;
	}

	// Places ball on top of car, moving forward — ground dribble & flick practice
	class BallOnCarState : public StateSetter {
	public:
		void ResetArena(Arena* arena) override {
			arena->ResetToRandomKickoff();
			auto cars = GetCarsVec(arena);
			if (cars.empty()) return;

			float x = RandFloat(-2500, 2500);
			float y = RandFloat(-3500, 3500);
			float yaw = RandFloat((float)-M_PI, (float)M_PI);
			Vec forward = { cosf(yaw), sinf(yaw), 0 };
			float speed = RandFloat(600, 1200);

			CarState cs = {};
			cs.pos = { x, y, 17 };
			cs.vel = forward * speed;
			cs.angVel = { 0, 0, 0 };
			cs.rotMat = Angle(yaw, 0, 0).ToRotMat();
			cs.boost = RandFloat(30, 100);
			cs.isOnGround = true;
			cars[0]->SetState(cs);

			BallState bs = {};
			bs.pos = { x, y, 170 };
			bs.vel = cs.vel;
			bs.angVel = { 0, 0, 0 };
			arena->ball->SetState(bs);

			if (cars.size() > 1) {
				CarState opp = {};
				float side = (cars[1]->team == Team::BLUE) ? -1.0f : 1.0f;
				opp.pos = { RandFloat(-1000, 1000), side * 4500, 17 };
				opp.vel = { 0, 0, 0 };
				opp.rotMat = Angle(0, 0, 0).ToRotMat();
				opp.boost = 33;
				opp.isOnGround = true;
				cars[1]->SetState(opp);
			}
		}
	};

	// Car and ball both airborne near wall — air dribble practice
	class AirDribbleSetup : public StateSetter {
	public:
		void ResetArena(Arena* arena) override {
			arena->ResetToRandomKickoff();
			auto cars = GetCarsVec(arena);
			if (cars.empty()) return;

			float side = (RandFloat(0, 1) > 0.5f) ? 1.0f : -1.0f;
			float y = RandFloat(-2000, 2000);
			float height = RandFloat(400, 900);
			float inwardX = -side * RandFloat(300, 600);
			float pitch = RandFloat(0.3f, 0.8f);
			float yaw = -side * (float)M_PI * 0.5f;

			CarState cs = {};
			cs.pos = { side * 3500 * 0.85f, y, height };
			cs.vel = { inwardX, RandFloat(-200, 200), RandFloat(200, 500) };
			cs.angVel = { 0, 0, 0 };
			cs.rotMat = Angle(yaw, pitch, 0).ToRotMat();
			cs.boost = RandFloat(40, 100);
			cs.isOnGround = false;
			cs.hasJumped = true;
			cs.hasDoubleJumped = false;
			cs.hasFlipped = false;
			cars[0]->SetState(cs);

			Vec fwd = cs.rotMat.forward;
			BallState bs = {};
			bs.pos = { cs.pos.x + fwd.x * 120, cs.pos.y + fwd.y * 120, cs.pos.z + 130 };
			bs.vel = cs.vel * 0.85f;
			bs.angVel = { 0, 0, 0 };
			arena->ball->SetState(bs);

			if (cars.size() > 1) {
				CarState opp = {};
				float oppSide = (cars[1]->team == Team::BLUE) ? -1.0f : 1.0f;
				opp.pos = { RandFloat(-500, 500), oppSide * 4500, 17 };
				opp.vel = { 0, 0, 0 };
				opp.rotMat = Angle(0, 0, 0).ToRotMat();
				opp.boost = 33;
				opp.isOnGround = true;
				cars[1]->SetState(opp);
			}
		}
	};

	// Car on ground near wall, ball on the wall surface rolling upward.
	// Forces the bot to drive up the wall and jump off toward the ball.
	// This is the natural bridge between ground play and aerials.
	class WallBallState : public StateSetter {
	public:
		void ResetArena(Arena* arena) override {
			arena->ResetToRandomKickoff();
			auto cars = GetCarsVec(arena);
			if (cars.empty()) return;

			// Pick a random side wall
			float side = (RandFloat(0, 1) > 0.5f) ? 1.0f : -1.0f;
			float y = RandFloat(-3000, 3000);

			// Car on ground near the wall, facing toward it
			float yaw = -side * (float)M_PI * 0.5f; // Face toward wall
			CarState cs = {};
			cs.pos = { side * 3200, y, 17 };
			cs.vel = { side * RandFloat(500, 1000), 0, 0 }; // Driving toward wall
			cs.angVel = { 0, 0, 0 };
			cs.rotMat = Angle(yaw, 0, 0).ToRotMat();
			cs.boost = RandFloat(50, 100);
			cs.isOnGround = true;
			cars[0]->SetState(cs);

			// Ball ON the wall surface with upward velocity.
			// Wall at x=±4096, ball radius ~93, so ball center at x=±(4096-93)=±4003.
			// Use ±3990 to avoid clipping into wall — physics will settle it.
			// Give upward velocity so ball rolls up the wall and stays there
			// long enough for the car to drive up and reach it.
			float ballHeight = RandFloat(200, 500);
			float ballY = y + RandFloat(-300, 300);
			float ballUpVel = RandFloat(400, 800); // Rolling upward on wall
			BallState bs = {};
			bs.pos = { side * 3990, ballY, ballHeight };
			bs.vel = { 0, RandFloat(-200, 200), ballUpVel };
			bs.angVel = { 0, 0, 0 };
			arena->ball->SetState(bs);

			if (cars.size() > 1) {
				CarState opp = {};
				float oppSide = (cars[1]->team == Team::BLUE) ? -1.0f : 1.0f;
				opp.pos = { RandFloat(-500, 500), oppSide * 4500, 17 };
				opp.vel = { 0, 0, 0 };
				opp.rotMat = Angle(0, 0, 0).ToRotMat();
				opp.boost = 33;
				opp.isOnGround = true;
				cars[1]->SetState(opp);
			}
		}
	};

	// Ball floating in air, car on ground — aerial practice for loose balls.
	// The bot must jump, boost upward, and hit the ball.
	class LooseAerialBallState : public StateSetter {
	public:
		void ResetArena(Arena* arena) override {
			arena->ResetToRandomKickoff();
			auto cars = GetCarsVec(arena);
			if (cars.empty()) return;

			// Ball in the air at a reachable height
			float ballX = RandFloat(-2500, 2500);
			float ballY = RandFloat(-3000, 3000);
			float ballZ = RandFloat(400, 1200);
			// Ball with some slow drift (not stationary, more realistic)
			float ballVx = RandFloat(-300, 300);
			float ballVy = RandFloat(-300, 300);
			float ballVz = RandFloat(0, 300); // Always drifting upward so ball stays airborne longer

			BallState bs = {};
			bs.pos = { ballX, ballY, ballZ };
			bs.vel = { ballVx, ballVy, ballVz };
			bs.angVel = { 0, 0, 0 };
			arena->ball->SetState(bs);

			// Car on ground, some distance from ball, facing roughly toward it
			float carDist = RandFloat(800, 2000);
			float carAngle = RandFloat((float)-M_PI, (float)M_PI);
			float carX = ballX + cosf(carAngle) * carDist;
			float carY = ballY + sinf(carAngle) * carDist;
			// Clamp to field bounds
			carX = RS_CLAMP(carX, -3800.0f, 3800.0f);
			carY = RS_CLAMP(carY, -4800.0f, 4800.0f);

			// Face toward ball
			float yaw = atan2f(ballY - carY, ballX - carX);

			CarState cs = {};
			cs.pos = { carX, carY, 17 };
			cs.vel = { cosf(yaw) * RandFloat(200, 800), sinf(yaw) * RandFloat(200, 800), 0 };
			cs.angVel = { 0, 0, 0 };
			cs.rotMat = Angle(yaw, 0, 0).ToRotMat();
			cs.boost = RandFloat(40, 100);
			cs.isOnGround = true;
			cars[0]->SetState(cs);

			if (cars.size() > 1) {
				CarState opp = {};
				float oppSide = (cars[1]->team == Team::BLUE) ? -1.0f : 1.0f;
				opp.pos = { RandFloat(-1000, 1000), oppSide * 4500, 17 };
				opp.vel = { 0, 0, 0 };
				opp.rotMat = Angle(0, 0, 0).ToRotMat();
				opp.boost = 33;
				opp.isOnGround = true;
				cars[1]->SetState(opp);
			}
		}
	};

	// Ball rolling toward car — catch & dribble practice
	class BallRollingToCarState : public StateSetter {
	public:
		void ResetArena(Arena* arena) override {
			arena->ResetToRandomKickoff();
			auto cars = GetCarsVec(arena);
			if (cars.empty()) return;

			float x = RandFloat(-2000, 2000);
			float y = RandFloat(-3000, 3000);
			float yaw = RandFloat((float)-M_PI, (float)M_PI);

			CarState cs = {};
			cs.pos = { x, y, 17 };
			cs.vel = { 0, 0, 0 };
			cs.rotMat = Angle(yaw, 0, 0).ToRotMat();
			cs.boost = RandFloat(30, 100);
			cs.isOnGround = true;
			cars[0]->SetState(cs);

			float ballDist = RandFloat(800, 1500);
			float ballAngle = yaw + RandFloat(-0.5f, 0.5f);
			float ballSpeed = RandFloat(400, 1200);

			BallState bs = {};
			bs.pos = { x + cosf(ballAngle) * ballDist, y + sinf(ballAngle) * ballDist, 93 };
			bs.vel = { -cosf(ballAngle) * ballSpeed, -sinf(ballAngle) * ballSpeed, 0 };
			bs.angVel = { 0, 0, 0 };
			arena->ball->SetState(bs);

			if (cars.size() > 1) {
				CarState opp = {};
				float oppSide = (cars[1]->team == Team::BLUE) ? -1.0f : 1.0f;
				opp.pos = { RandFloat(-1000, 1000), oppSide * 4500, 17 };
				opp.vel = { 0, 0, 0 };
				opp.rotMat = Angle(0, 0, 0).ToRotMat();
				opp.boost = 33;
				opp.isOnGround = true;
				cars[1]->SetState(opp);
			}
		}
	};
}
