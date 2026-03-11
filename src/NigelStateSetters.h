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
