// PhysicsWorld: joints (JointComponent constraints, motors, breaking).
#include "Basalt/Physics/PhysicsWorld.h"

#include "Basalt/Physics/JointSettings.h"
#include "Basalt/Physics/PhysicsWorldImpl.h"
#include "Basalt/Scene/JointFields.h"
#include "Basalt/Scripting/ScriptEngine.h"

#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Basalt {

	namespace {

		// The second body's joint frame seen from the first's (see JointRecord::RestRotation).
		JPH::Quat RelativeJointRotation(const JPH::TwoBodyConstraint& constraint)
		{
			const JPH::Quat frame1 = constraint.GetBody1()->GetRotation() * constraint.GetConstraintToBody1Matrix().GetQuaternion();
			const JPH::Quat frame2 = constraint.GetBody2()->GetRotation() * constraint.GetConstraintToBody2Matrix().GetQuaternion();
			return (frame1.Conjugated() * frame2).Normalized();
		}

	}

	void PhysicsWorld::Impl::UpdateIgnoredPairs()
	{
		std::set<EntityPair> pairs;
		for (const auto& [holder, joint] : Joints)
		{
			if (joint.Settings.ConnectedEntity == 0 || joint.Settings.EnableCollision)
				continue;
			const uint64_t a = static_cast<uint64_t>(joint.Body);
			const uint64_t b = static_cast<uint64_t>(joint.Settings.ConnectedEntity);
			pairs.emplace(std::min(a, b), std::max(a, b));
		}
		// Jolt replays cached contacts for bodies that have not moved (and skips sleeping ones), bypassing
		// the pair filter. Bodies whose pair starts or stops being ignored lose that cache and wake, so two
		// resting bodies stop (or start) colliding at once.
		std::vector<EntityPair> changed;
		std::ranges::set_symmetric_difference(pairs, IgnoredPairs, std::back_inserter(changed));
		IgnoredPairs = std::move(pairs);
		JPH::BodyInterface& bodies = System->GetBodyInterface();
		for (const auto& [a, b] : changed)
		{
			for (uint64_t uuid : { a, b })
			{
				auto body = Bodies.find(uuid);
				if (body == Bodies.end())
					continue;
				bodies.InvalidateContactCache(body->second.ID);
				if (body->second.Type == RigidBodyType::Dynamic)
					bodies.ActivateBody(body->second.ID);
			}
		}
	}

	void PhysicsWorld::Impl::RemoveJoint(UUID holder)
	{
		auto it = Joints.find(holder);
		if (it == Joints.end())
			return;
		System->RemoveConstraint(it->second.Constraint);
		Joints.erase(it);
		UpdateIgnoredPairs();
	}

	void PhysicsWorld::Impl::RemoveJointsOfBody(UUID uuid)
	{
		// Jolt removes a constraint by moving its last one into the gap, which changes the solve order of
		// the rest; removing in UUID order (not hash order) keeps that order reproducible.
		std::vector<UUID> holders;
		for (const auto& [holder, joint] : Joints)
		{
			if (joint.Body == uuid || joint.Settings.ConnectedEntity == uuid)
				holders.push_back(holder);
		}
		std::ranges::sort(holders);
		for (UUID holder : holders)
		{
			auto it = Joints.find(holder);
			System->RemoveConstraint(it->second.Constraint);
			DirtyJoints.insert(holder);
			Joints.erase(it);
		}
		UpdateIgnoredPairs();
	}

	void PhysicsWorld::Impl::OnJointChanged(entt::registry& registry, entt::entity entity)
	{
		if (const auto* id = registry.try_get<IDComponent>(entity))
			DirtyJoints.insert(id->ID);
	}

	void PhysicsWorld::Impl::OnJointDestroyed(entt::registry& registry, entt::entity entity)
	{
		if (const auto* id = registry.try_get<IDComponent>(entity))
		{
			RemoveJoint(id->ID);
			DirtyJoints.erase(id->ID);
			JointWarnings.Forget(id->ID);
		}
	}

	void PhysicsWorld::MarkJointsDirty(UUID uuid)
	{
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			const JointComponent& joint = entity.GetComponent<JointComponent>();
			if (JointBody(entity.GetUUID(), joint) == uuid || joint.ConnectedEntity == uuid)
				m_Impl->DirtyJoints.insert(entity.GetUUID());
		}
	}

	void PhysicsWorld::RebuildDirtyJoints()
	{
		Impl& impl = *m_Impl;
		if (impl.DirtyJoints.empty())
			return;
		// Walk the registry rather than the (unordered) dirty set so constraints are added in a stable
		// order: Jolt solves them in that order, and replays must not depend on hash-set iteration.
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			// An entity destroyed during this frame's scripts is still here until the end of the frame.
			if (!impl.DirtyJoints.contains(entity.GetUUID()) || m_Scene->IsEntityPendingDestruction(entity))
				continue;
			// Scripts drive motors by setting the component every frame. Rebuilding would reset the
			// constraint's rest pose and warm start, so only structural changes rebuild it.
			auto existing = impl.Joints.find(entity.GetUUID());
			const JointComponent& joint = entity.GetComponent<JointComponent>();
			std::vector<std::string> warnings;
			if (existing != impl.Joints.end() && !JointNeedsRebuild(existing->second.Settings, joint))
			{
				// Rewriting the same values must not wake the bodies.
				if (joint == existing->second.Settings)
					continue;
				warnings = existing->second.BuildWarnings;
				ApplyJointSettings(entity, warnings);
			}
			else
			{
				CreateJoint(entity, warnings);
			}
			impl.JointWarnings.Report(entity, warnings);
		}
		impl.DirtyJoints.clear();
		impl.UpdateIgnoredPairs();
	}

	void PhysicsWorld::ApplyJointSettings(Entity entity, std::vector<std::string>& warnings)
	{
		Impl::JointRecord& record = m_Impl->Joints.at(entity.GetUUID());
		const JointComponent& joint = entity.GetComponent<JointComponent>();
		record.Settings = joint;

		ReportIgnoredFields(joint, warnings);
		const auto [limitMin, limitMax] = SanitizeLimits(joint, warnings);
		const JPH::SpringSettings limitSpring = SanitizeLimitSpring(joint, warnings);
		if (joint.MotorMaxForce < 0.0f)
			warnings.emplace_back(fmt::format("MotorMaxForce {} is negative; using 0", joint.MotorMaxForce));
		const JPH::EMotorState motorState = ToJoltMotorState(joint.MotorMode);
		const float motorLimit = std::max(joint.MotorMaxForce, 0.0f);
		const JPH::SpringSettings motorSpring = SanitizeMotorSpring(joint, warnings);

		switch (joint.Type)
		{
			case JointType::Hinge:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Hinge, "joint record out of sync with its constraint");
				auto* hinge = static_cast<JPH::HingeConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					hinge->SetLimits(glm::radians(limitMin), glm::radians(limitMax));
				hinge->SetLimitsSpringSettings(limitSpring);
				hinge->GetMotorSettings().SetTorqueLimit(motorLimit);
				hinge->GetMotorSettings().mSpringSettings = motorSpring;
				hinge->SetMotorState(motorState);
				if (motorState == JPH::EMotorState::Velocity)
				{
					hinge->SetTargetAngularVelocity(glm::radians(joint.MotorTarget));
				}
				else if (motorState == JPH::EMotorState::Position)
				{
					if (joint.MotorTarget < -180.0f || joint.MotorTarget > 180.0f)
						warnings.emplace_back("hinge position motor targets must be within [-180, 180] degrees; clamped");
					hinge->SetTargetAngle(glm::radians(std::clamp(joint.MotorTarget, -180.0f, 180.0f)));
				}
				break;
			}
			case JointType::Slider:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Slider, "joint record out of sync with its constraint");
				auto* slider = static_cast<JPH::SliderConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					slider->SetLimits(limitMin, limitMax);
				slider->SetLimitsSpringSettings(limitSpring);
				slider->GetMotorSettings().SetForceLimit(motorLimit);
				slider->GetMotorSettings().mSpringSettings = motorSpring;
				slider->SetMotorState(motorState);
				if (motorState == JPH::EMotorState::Velocity)
					slider->SetTargetVelocity(joint.MotorTarget);
				else if (motorState == JPH::EMotorState::Position)
					slider->SetTargetPosition(joint.MotorTarget);
				break;
			}
			case JointType::Distance:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Distance, "joint record out of sync with its constraint");
				auto* distance = static_cast<JPH::DistanceConstraint*>(record.Constraint.GetPtr());
				if (joint.UseLimits)
					distance->SetDistance(limitMin, limitMax);
				distance->SetLimitsSpringSettings(limitSpring);
				break;
			}
			case JointType::Cone:
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::Cone, "joint record out of sync with its constraint");
				// Jolt requires at most pi, which glm::radians(180) can round past.
				static_cast<JPH::ConeConstraint*>(record.Constraint.GetPtr())->SetHalfConeAngle(joint.UseLimits ? std::min(glm::radians(limitMax), JPH::JPH_PI) : JPH::JPH_PI);
				break;
			case JointType::SixDOF:
			{
				BS_CORE_ASSERT(record.Constraint->GetSubType() == JPH::EConstraintSubType::SixDOF, "joint record out of sync with its constraint");
				auto* sixDOF = static_cast<JPH::SixDOFConstraint*>(record.Constraint.GetPtr());
				const SixDOFLimits limits = SanitizeSixDOFLimits(joint, warnings);
				sixDOF->SetTranslationLimits(ToJolt(limits.LinearMin), ToJolt(limits.LinearMax));
				sixDOF->SetRotationLimits(ToJolt(limits.AngularMin), ToJolt(limits.AngularMax));
				// Jolt softens translation limits only; rotation limits stay rigid. A spring on a locked axis would
				// make the lock soft too, so only limited axes get it.
				using EAxis = JPH::SixDOFConstraint::EAxis;
				bool softened = false;
				for (int i = 0; i < 3; i++)
				{
					const auto axis = static_cast<EAxis>(EAxis::TranslationX + i);
					const bool limited = !sixDOF->IsFixedAxis(axis) && !sixDOF->IsFreeAxis(axis);
					sixDOF->SetLimitsSpringSettings(axis, limited ? limitSpring : JPH::SpringSettings());
					softened |= limited;
				}
				if (limitSpring.HasStiffness() && joint.UseLimits && !softened)
					warnings.emplace_back("LimitSpringFrequency has no effect on a six-DOF joint without a limited translation axis (rotation limits and locked or free axes stay rigid)");

				// Per-axis motors in the joint frame. Jolt keeps one target vector per kind, so each axis takes
				// its own component and the rest are 0; the Position rotation axes share one target orientation.
				glm::vec3 linearVelocity(0.0f);
				glm::vec3 linearPosition(0.0f);
				glm::vec3 angularVelocity(0.0f);
				glm::vec3 angularPosition(0.0f);
				for (int i = 0; i < 3; i++)
				{
					const auto linearAxis = static_cast<EAxis>(EAxis::TranslationX + i);
					const auto angularAxis = static_cast<EAxis>(EAxis::RotationX + i);
					// Jolt skips motors only when a whole group (translation or rotation) is locked; a motor on one
					// locked axis would fight the lock, so it stays off.
					JointMotorMode linearMode = joint.LinearMotorMode[i];
					JointMotorMode angularMode = joint.AngularMotorMode[i];
					if (linearMode != JointMotorMode::Off && sixDOF->IsFixedAxis(linearAxis))
					{
						warnings.emplace_back(fmt::format("six-DOF linear {} motor has no effect: the axis is locked{}", AxisNames[i], joint.UseLimits ? "" : " (translation is locked without UseLimits)"));
						linearMode = JointMotorMode::Off;
					}
					if (angularMode != JointMotorMode::Off && sixDOF->IsFixedAxis(angularAxis))
					{
						warnings.emplace_back(fmt::format("six-DOF angular {} motor has no effect: the axis is locked", AxisNames[i]));
						angularMode = JointMotorMode::Off;
					}
					if (linearMode == JointMotorMode::Velocity)
						linearVelocity[i] = joint.LinearMotorTarget[i];
					else if (linearMode == JointMotorMode::Position)
					{
						// Jolt clamps only the orientation target; past a limit the motor would push against it at
						// MotorMaxForce indefinitely.
						linearPosition[i] = joint.LinearMotorTarget[i];
						if (!sixDOF->IsFreeAxis(linearAxis))
							linearPosition[i] = std::clamp(linearPosition[i], limits.LinearMin[i], limits.LinearMax[i]);
					}
					if (angularMode == JointMotorMode::Velocity)
						angularVelocity[i] = joint.AngularMotorTarget[i];
					else if (angularMode == JointMotorMode::Position)
						angularPosition[i] = joint.AngularMotorTarget[i];

					for (const auto axis : { linearAxis, angularAxis })
					{
						JPH::MotorSettings& motor = sixDOF->GetMotorSettings(axis);
						motor.SetForceLimit(motorLimit);
						motor.SetTorqueLimit(motorLimit);
						motor.mSpringSettings = motorSpring;
					}
					sixDOF->SetMotorState(linearAxis, ToJoltMotorState(linearMode));
					sixDOF->SetMotorState(angularAxis, ToJoltMotorState(angularMode));
				}
				sixDOF->SetTargetVelocityCS(ToJolt(linearVelocity));
				sixDOF->SetTargetPositionCS(ToJolt(linearPosition));
				sixDOF->SetTargetAngularVelocityCS(ToJolt(glm::radians(angularVelocity)));
				// Jolt clamps the orientation to the rotation limits.
				sixDOF->SetTargetOrientationCS(ToJolt(Math::QuatFromEuler(glm::radians(angularPosition))));
				break;
			}
			case JointType::Fixed:
			case JointType::Point:
				break;
		}

		// Sleeping bodies would ignore a new motor target or limit.
		JPH::BodyInterface& bodies = m_Impl->System->GetBodyInterface();
		for (const JPH::Body* body : { record.Constraint->GetBody1(), record.Constraint->GetBody2() })
		{
			if (body && !body->IsStatic())
				bodies.ActivateBody(body->GetID());
		}
	}

	void PhysicsWorld::CheckBrokenJoints(float fixedStep)
	{
		Impl& impl = *m_Impl;
		if (impl.Joints.empty())
			return;

		// Constraint lambdas are the impulses applied during the last step; divided by the step they are
		// the force (N) and torque (N·m) the joint needed to hold. Hinge and slider limits and motors act
		// along the joint's free axis, so they add to the force or torque the joint carries.
		struct BrokenJoint
		{
			UUID Owner;
			float Force = 0.0f;
			float Torque = 0.0f;
		};
		auto exceeds = [](float value, float limit) { return limit > 0.0f && value > limit; };
		auto combine = [](float perpendicular, float axial) { return std::sqrt(perpendicular * perpendicular + axial * axial); };
		std::vector<BrokenJoint> broken;
		for (entt::entity handle : m_Scene->GetAllEntitiesWith<JointComponent>())
		{
			Entity entity(handle, m_Scene);
			auto it = impl.Joints.find(entity.GetUUID());
			if (it == impl.Joints.end())
				continue;
			const Impl::JointRecord& record = it->second;
			const float breakForce = record.Settings.BreakForce;
			const float breakTorque = record.Settings.BreakTorque;
			if (breakForce <= 0.0f && breakTorque <= 0.0f)
				continue;

			float forceImpulse = 0.0f;
			float torqueImpulse = 0.0f;
			const JPH::Constraint* constraint = record.Constraint.GetPtr();
			switch (record.Settings.Type)
			{
				case JointType::Fixed:
				{
					const auto* fixed = static_cast<const JPH::FixedConstraint*>(constraint);
					forceImpulse = fixed->GetTotalLambdaPosition().Length();
					torqueImpulse = fixed->GetTotalLambdaRotation().Length();
					break;
				}
				case JointType::Point:
					forceImpulse = static_cast<const JPH::PointConstraint*>(constraint)->GetTotalLambdaPosition().Length();
					break;
				case JointType::Hinge:
				{
					const auto* hinge = static_cast<const JPH::HingeConstraint*>(constraint);
					forceImpulse = hinge->GetTotalLambdaPosition().Length();
					torqueImpulse = combine(hinge->GetTotalLambdaRotation().Length(), hinge->GetTotalLambdaRotationLimits() + hinge->GetTotalLambdaMotor());
					break;
				}
				case JointType::Slider:
				{
					const auto* slider = static_cast<const JPH::SliderConstraint*>(constraint);
					forceImpulse = combine(slider->GetTotalLambdaPosition().Length(), slider->GetTotalLambdaPositionLimits() + slider->GetTotalLambdaMotor());
					torqueImpulse = slider->GetTotalLambdaRotation().Length();
					break;
				}
				case JointType::Distance:
					forceImpulse = std::abs(static_cast<const JPH::DistanceConstraint*>(constraint)->GetTotalLambdaPosition());
					break;
				case JointType::Cone:
				{
					const auto* cone = static_cast<const JPH::ConeConstraint*>(constraint);
					forceImpulse = cone->GetTotalLambdaPosition().Length();
					torqueImpulse = std::abs(cone->GetTotalLambdaRotation());
					break;
				}
				case JointType::SixDOF:
				{
					// Motors only run while a group is partly free, and then both lambdas are per joint-frame axis, so
					// they add component-wise (like a hinge's limit and motor lambdas).
					const auto* sixDOF = static_cast<const JPH::SixDOFConstraint*>(constraint);
					forceImpulse = (sixDOF->GetTotalLambdaPosition() + sixDOF->GetTotalLambdaMotorTranslation()).Length();
					torqueImpulse = (sixDOF->GetTotalLambdaRotation() + sixDOF->GetTotalLambdaMotorRotation()).Length();
					break;
				}
			}
			const float force = forceImpulse / fixedStep;
			const float torque = torqueImpulse / fixedStep;
			if (exceeds(force, breakForce) || exceeds(torque, breakTorque))
				broken.push_back({ entity.GetUUID(), force, torque });
		}

		// A broken joint is gone for good: its component is removed, so the scene state (and scene.hash)
		// records the break and replays stay consistent.
		ScriptEngine* scriptEngine = m_Scene->GetScriptEngine();
		for (const BrokenJoint& joint : broken)
		{
			// An earlier OnJointBreak callback may have removed this joint or its entity.
			Entity entity = m_Scene->GetEntityByUUID(joint.Owner);
			auto it = impl.Joints.find(joint.Owner);
			if (!entity || it == impl.Joints.end() || !entity.HasComponent<JointComponent>())
				continue;
			const JointComponent& settings = it->second.Settings;
			// The measured values help tune thresholds.
			BS_CORE_INFO("Physics: joint on '{}' broke (force {:.1f} N, BreakForce {}; torque {:.1f} N·m, BreakTorque {})", entity.GetName(), joint.Force, settings.BreakForce, joint.Torque, settings.BreakTorque);
			const UUID connectedID = settings.ConnectedEntity;
			const UUID bodyID = it->second.Body;
			entity.RemoveComponent<JointComponent>();
			if (scriptEngine)
				scriptEngine->OnJointBroken(entity, m_Scene->GetEntityByUUID(bodyID), m_Scene->GetEntityByUUID(connectedID));
		}
	}

	void PhysicsWorld::CreateJoint(Entity entity, std::vector<std::string>& warnings)
	{
		Impl& impl = *m_Impl;
		impl.RemoveJoint(entity.GetUUID());
		const JointComponent& joint = entity.GetComponent<JointComponent>();

		// A RigidBody without a valid collider has no body either; say which part is missing.
		auto describeMissingBody = [](Entity e) {
			if (e.HasComponent<CharacterControllerComponent>())
				return "is a character (a CharacterController replaces its body; joints need a RigidBody)";
			return e.HasComponent<RigidBodyComponent>() ? "has a RigidBody but no valid collider" : "has no RigidBody";
		};
		// The joint moves BodyEntity's body; its anchor and axes are in that entity's local space. The holder
		// itself always exists, so only a BodyEntity can be missing.
		Entity bodyEntity = m_Scene->GetEntityByUUID(JointBody(entity.GetUUID(), joint));
		if (!bodyEntity)
		{
			warnings.emplace_back(fmt::format("not built: its BodyEntity {} is missing", static_cast<uint64_t>(joint.BodyEntity)));
			return;
		}
		auto self = impl.Bodies.find(bodyEntity.GetUUID());
		if (self == impl.Bodies.end())
		{
			if (bodyEntity == entity)
				warnings.emplace_back(fmt::format("not built: the entity {}", describeMissingBody(entity)));
			else
				warnings.emplace_back(fmt::format("not built: its BodyEntity '{}' {}", bodyEntity.GetName(), describeMissingBody(bodyEntity)));
			return;
		}

		JPH::BodyID bodyIDs[2] = { JPH::BodyID(), self->second.ID };
		RigidBodyType connectedType = RigidBodyType::Static;
		Entity connected;
		if (joint.ConnectedEntity != 0)
		{
			connected = m_Scene->GetEntityByUUID(joint.ConnectedEntity);
			if (!connected || connected == bodyEntity)
			{
				warnings.emplace_back(fmt::format("not built: it connects to {} entity {}", connected ? "its own body's" : "a missing", static_cast<uint64_t>(joint.ConnectedEntity)));
				return;
			}
			auto other = impl.Bodies.find(joint.ConnectedEntity);
			if (other == impl.Bodies.end())
			{
				warnings.emplace_back(fmt::format("not built: the connected entity '{}' {}", connected.GetName(), describeMissingBody(connected)));
				return;
			}
			bodyIDs[0] = other->second.ID;
			connectedType = other->second.Type;
		}
		// Jolt only moves dynamic bodies; a constraint between static and kinematic bodies does nothing.
		if (self->second.Type != RigidBodyType::Dynamic && connectedType != RigidBodyType::Dynamic)
		{
			warnings.emplace_back("not built: it connects no dynamic body (only static, kinematic or the world) and would have no effect");
			return;
		}

		// The joint frame in world space. Both bodies share it at creation, so their current relative pose
		// becomes the joint's rest pose. The axis turns with the body's rotation only: Jolt bodies are
		// unscaled, so a scaled world matrix would skew it under non-uniform scale.
		const glm::mat4 world = m_Scene->GetWorldTransform(bodyEntity);
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(world, position, rotation, scale))
		{
			warnings.emplace_back(fmt::format("not built: '{}' has a degenerate transform", bodyEntity.GetName()));
			return;
		}
		const glm::vec3 anchor = glm::vec3(world * glm::vec4(joint.Anchor, 1.0f));
		glm::vec3 axis = joint.Axis;
		if (glm::length(axis) < 1e-6f)
		{
			// Fixed, point and distance joints have no axis.
			if (joint.Type != JointType::Fixed && joint.Type != JointType::Point && joint.Type != JointType::Distance)
				warnings.emplace_back("Axis is zero; using local Y");
			axis = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		const glm::vec3 worldAxis = glm::normalize(rotation * axis);
		const JPH::Vec3 joltAxis = ToJolt(worldAxis);
		const JPH::Vec3 joltNormal = joltAxis.GetNormalizedPerpendicular();

		// Limits, motors and break thresholds are applied by ApplyJointSettings below.
		JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
		switch (joint.Type)
		{
			case JointType::Fixed:
			{
				auto* fixed = new JPH::FixedConstraintSettings();
				fixed->mPoint1 = fixed->mPoint2 = ToJolt(anchor);
				settings = fixed;
				break;
			}
			case JointType::Point:
			{
				auto* point = new JPH::PointConstraintSettings();
				point->mPoint1 = point->mPoint2 = ToJolt(anchor);
				settings = point;
				break;
			}
			case JointType::Hinge:
			{
				auto* hinge = new JPH::HingeConstraintSettings();
				hinge->mPoint1 = hinge->mPoint2 = ToJolt(anchor);
				hinge->mHingeAxis1 = hinge->mHingeAxis2 = joltAxis;
				hinge->mNormalAxis1 = hinge->mNormalAxis2 = joltNormal;
				settings = hinge;
				break;
			}
			case JointType::Slider:
			{
				auto* slider = new JPH::SliderConstraintSettings();
				slider->mPoint1 = slider->mPoint2 = ToJolt(anchor);
				slider->mSliderAxis1 = slider->mSliderAxis2 = joltAxis;
				slider->mNormalAxis1 = slider->mNormalAxis2 = joltNormal;
				settings = slider;
				break;
			}
			case JointType::Distance:
			{
				auto* distance = new JPH::DistanceConstraintSettings();
				const glm::vec3 connectedAnchor = connected ? glm::vec3(m_Scene->GetWorldTransform(connected) * glm::vec4(joint.ConnectedAnchor, 1.0f)) : joint.ConnectedAnchor;
				distance->mPoint1 = ToJolt(connectedAnchor);
				distance->mPoint2 = ToJolt(anchor);
				// Jolt keeps the starting distance (its default min/max of -1 mean "current"); limits replace it.
				settings = distance;
				break;
			}
			case JointType::Cone:
			{
				auto* cone = new JPH::ConeConstraintSettings();
				cone->mPoint1 = cone->mPoint2 = ToJolt(anchor);
				cone->mTwistAxis1 = cone->mTwistAxis2 = joltAxis;
				cone->mHalfConeAngle = JPH::JPH_PI;
				settings = cone;
				break;
			}
			case JointType::SixDOF:
			{
				// The frame's X is Axis; Y is SecondaryAxis with its Axis component removed.
				glm::vec3 secondary = rotation * joint.SecondaryAxis;
				secondary -= glm::dot(secondary, worldAxis) * worldAxis;
				JPH::Vec3 joltSecondary = joltNormal;
				if (glm::length(secondary) < 1e-4f)
					warnings.emplace_back("SecondaryAxis is zero or parallel to Axis; using an arbitrary perpendicular");
				else
					joltSecondary = ToJolt(glm::normalize(secondary));
				auto* sixDOF = new JPH::SixDOFConstraintSettings();
				sixDOF->mPosition1 = sixDOF->mPosition2 = ToJolt(anchor);
				sixDOF->mAxisX1 = sixDOF->mAxisX2 = joltAxis;
				sixDOF->mAxisY1 = sixDOF->mAxisY2 = joltSecondary;
				sixDOF->mSwingType = JPH::ESwingType::Cone;
				settings = sixDOF;
				break;
			}
		}

		// Body 1 is the connected body (or the world), body 2 the joint's body (BodyEntity's, or the holder's).
		const int bodyCount = connected ? 2 : 1;
		JPH::BodyLockMultiWrite lock(impl.System->GetBodyLockInterface(), connected ? bodyIDs : bodyIDs + 1, bodyCount);
		JPH::Body* body1 = connected ? lock.GetBody(0) : &JPH::Body::sFixedToWorld;
		JPH::Body* body2 = lock.GetBody(bodyCount - 1);
		if (!body1 || !body2)
		{
			// Both bodies are in Bodies, so Jolt should always find them.
			BS_CORE_ERROR("Physics: joint on '{}' not built: its bodies could not be locked", entity.GetName());
			return;
		}

		Impl::JointRecord& record = impl.Joints[entity.GetUUID()];
		record.Constraint = settings->Create(*body1, *body2);
		record.Settings = joint;
		record.Body = bodyEntity.GetUUID();
		record.BuildWarnings = warnings;
		record.RestRotation = RelativeJointRotation(*record.Constraint);
		impl.System->AddConstraint(record.Constraint);
		lock.ReleaseLocks();
		ApplyJointSettings(entity, warnings);
	}

	bool PhysicsWorld::HasJoint(Entity entity) const
	{
		return entity && m_Impl->Joints.contains(entity.GetUUID());
	}

	std::optional<float> PhysicsWorld::GetJointPosition(Entity entity) const
	{
		if (!entity)
			return std::nullopt;
		auto it = m_Impl->Joints.find(entity.GetUUID());
		if (it == m_Impl->Joints.end())
			return std::nullopt;
		const JPH::Constraint* constraint = it->second.Constraint.GetPtr();
		switch (it->second.Settings.Type)
		{
			case JointType::Hinge:
				return glm::degrees(static_cast<const JPH::HingeConstraint*>(constraint)->GetCurrentAngle());
			case JointType::Slider:
				return static_cast<const JPH::SliderConstraint*>(constraint)->GetCurrentPosition();
			case JointType::Fixed:
			case JointType::Point:
			case JointType::Distance:
			case JointType::Cone:
			case JointType::SixDOF:
				break;
		}
		return std::nullopt;
	}

	std::optional<glm::vec3> PhysicsWorld::GetJointRotation(Entity entity) const
	{
		if (!entity)
			return std::nullopt;
		auto it = m_Impl->Joints.find(entity.GetUUID());
		if (it == m_Impl->Joints.end())
			return std::nullopt;
		const JointType type = it->second.Settings.Type;
		if (type != JointType::Cone && type != JointType::SixDOF)
			return std::nullopt;
		// Removing the rest offset on the right keeps the result in the first body's joint frame.
		const Impl::JointRecord& record = it->second;
		const JPH::Quat rotation = RelativeJointRotation(*record.Constraint) * record.RestRotation.Conjugated();
		return glm::degrees(Math::EulerFromQuat(FromJolt(rotation.Normalized())));
	}

}
