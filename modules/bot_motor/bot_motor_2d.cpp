#include "bot_motor_2d.h"

#include "core/config/engine.h"
#include "core/profiling/profiling.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "scene/resources/world_2d.h"
#include "scene/resources/curve.h"
#include "core/io/resource_loader.h"
#include "scene/2d/animated_sprite_2d.h"
#include "scene/2d/camera_2d.h"
#include "scene/2d/navigation/navigation_agent_2d.h"
#include "scene/2d/physics/character_body_2d.h"
#include "scene/main/multiplayer_api.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/physics_2d/physics_server_2d.h"


void BotMotor2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("register_bot", "body"), &BotMotor2D::register_bot);
	ClassDB::bind_method(D_METHOD("unregister_bot", "body"), &BotMotor2D::unregister_bot);
	ClassDB::bind_method(D_METHOD("get_active_count"), &BotMotor2D::get_active_count);
	ClassDB::bind_method(D_METHOD("register_composer", "composer"), &BotMotor2D::register_composer);
	ClassDB::bind_method(D_METHOD("register_projectile", "projectile", "velocity", "enabled"), &BotMotor2D::register_projectile);
	ClassDB::bind_method(D_METHOD("select_best_target", "composer"), &BotMotor2D::select_best_target);
	ClassDB::bind_method(D_METHOD("has_clear_sight", "character", "from", "to"), &BotMotor2D::has_clear_sight);
	ClassDB::bind_method(D_METHOD("reset_composer", "composer"), &BotMotor2D::reset_composer);
	ClassDB::bind_static_method("BotMotor2D", D_METHOD("wall_avoidance", "space", "exclude", "origin", "direction", "half_spread", "detect", "count", "mask", "push_strength", "max_length"), &BotMotor2D::wall_avoidance);
}

BotMotor2D::BotMotor2D() {
	set_physics_process_priority(-1);
}

void BotMotor2D::register_bot(Node *p_body) {
	ERR_FAIL_NULL(p_body);
	const ObjectID id = p_body->get_instance_id();
	for (const Entry &e : bots) {
		if (e.id == id) {
			return;
		}
	}
	Entry e;
	e.id = id;
	e.has_footsteps = p_body->has_method(SN_FOOTSTEPS);
	for (Ref<Script> s = p_body->get_script(); s.is_valid(); s = s->get_base_script()) {
		if (s->get_global_name() == SN_FUNGUS_RED) {
			e.fungus_anim = true;
			break;
		}
	}
	bots.push_back(e);
}

void BotMotor2D::unregister_bot(Node *p_body) {
	ERR_FAIL_NULL(p_body);
	const ObjectID id = p_body->get_instance_id();
	for (uint32_t i = 0; i < bots.size(); i++) {
		if (bots[i].id == id) {
			if (bots[i].owned && p_body->is_inside_tree()) {
				p_body->set_physics_process(true);
			}
			bots.remove_at_unordered(i);
			return;
		}
	}
}

void BotMotor2D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_physics_process_internal(true);
			set_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_tick_brains(get_process_delta_time());
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_tick_shots(get_physics_process_delta_time());
			_step(get_physics_process_delta_time());
		} break;
	}
}

void BotMotor2D::_step(double p_delta) {
	GodotProfileZone("motor _step");
	bool frozen = false;
	bool slowmo_bots = true;
	if (is_inside_tree()) {
		Node *hitstop = get_tree()->get_root()->get_node_or_null(NodePath("HitstopManager"));
		if (hitstop) {
			frozen = hitstop->get(SN_IS_FROZEN);
			slowmo_bots = hitstop->get(SN_ENABLE_SLOWMO_BOTS);
		}
	}

	const int interval = CLAMP((active_count + bots_per_step_interval - 1) / MAX(bots_per_step_interval, 1), 1, max_step_interval);
	const uint64_t physics_frame = Engine::get_singleton()->get_physics_frames();
	const float delta = p_delta;

	Viewport *view_vp = nullptr;
	Rect2 view_rect;
	bool view_rect_valid = false;

	int active = 0;
	for (uint32_t i = 0; i < bots.size();) {
		Entry &entry = bots[i];
		CharacterBody2D *body = ObjectDB::get_instance<CharacterBody2D>(entry.id);
		if (!body) {
			bots.remove_at_unordered(i);
			continue;
		}
		i++;
		if (!body->is_inside_tree() || !body->can_process()) {
			continue;
		}
		Ref<MultiplayerAPI> mp = body->get_multiplayer();
		const bool eligible = mp.is_valid() && mp->has_multiplayer_peer() && body->is_multiplayer_authority() && bool(body->get(SN_IS_BOT));
		if (!eligible) {
			if (entry.owned) {
				entry.owned = false;
				body->set_physics_process(true);
			}
			continue;
		}
		if (body->is_physics_processing()) {
			body->set_physics_process(false);
		}
		entry.owned = true;

		if (!bool(body->get(SN_IS_ALIVE))) {
			continue;
		}
		active++;

		Vector2 velocity = body->get_velocity();

		if (!frozen && interval > 1 && (physics_frame + uint64_t(int64_t(body->get(SN_IDX)))) % interval != 0 && velocity.length() * delta * (interval - 1) < max_drift) {
			body->set_position(body->get_position() + velocity * delta);
			body->set(SN_REPLICATED_POSITION, body->get_position());
			continue;
		}

		const bool is_walking = body->get(SN_IS_WALKING);

		if (!(frozen && !slowmo_bots)) {
			Object *input = body->get(SN_INPUT);
			if (input) {
				const Vector2 input_dir = input->get(SN_MOVEMENT);
				Vector2 target_vel;
				if (input_dir.length_squared() > 0.0001f) {
					float cap = float(int64_t(body->get(SN_CURRENT_MAX_VELOCITY)));
					if (is_walking) {
						cap *= 0.5f;
					}
					target_vel = input_dir.limit_length(1.0) * cap;
				}
				if (is_walking) {
					velocity = target_vel;
				} else {
					const float rate = target_vel.length_squared() > 0.0001f ? float(body->get(SN_RUN_ACCELERATION)) : float(body->get(SN_RUN_DECELERATION));
					velocity = velocity.move_toward(target_vel, rate * delta);
				}
				body->set_velocity(velocity);
				if (velocity.length_squared() > 1.0f || target_vel.length_squared() > 1.0f) {
					body->move_and_slide();
				}
				body->set(SN_REPLICATED_VELOCITY, body->get_velocity());
				body->set(SN_REPLICATED_POSITION, body->get_position());
			}
		}

		Viewport *vp = body->get_viewport();
		if (vp != view_vp) {
			view_vp = vp;
			view_rect_valid = false;
			Camera2D *cam = vp ? vp->get_camera_2d() : nullptr;
			if (cam && cam->get_zoom().x > 0.0f && cam->get_zoom().y > 0.0f) {
				const Vector2 size = vp->get_visible_rect().size / cam->get_zoom();
				const Vector2 grown = size * (1.0f + 2.0f * view_margin);
				view_rect = Rect2(cam->get_camera_screen_center() - grown * 0.5f, grown);
				view_rect_valid = true;
			}
		}
		const bool in_view = !view_rect_valid || view_rect.has_point(body->get_global_position());
		if (!in_view) {
			continue;
		}
		AnimatedSprite2D *sprite = Object::cast_to<AnimatedSprite2D>(body->get(SN_SPRITE).get_validated_object());
		if (!sprite) {
			continue;
		}
		if (entry.fungus_anim) {
			const Vector2 rv = body->get(SN_REPLICATED_VELOCITY);
			const Vector2 look = body->get(SN_LOOKING_AT);
			if (Math::abs(rv.x) > 5.0f) {
				sprite->set_flip_h(rv.x < 0.0f);
			} else if (look != Vector2()) {
				sprite->set_flip_h(look.x < body->get_global_position().x);
			}
			const StringName &fungus_anim = int64_t(OS::get_singleton()->get_ticks_msec()) < int64_t(body->get(SN_SHOT_ANIM_UNTIL)) ? SN_ATTACK : (rv.length() > 5.0f ? SN_WALK : SN_IDLE);
			if (sprite->get_animation() != fungus_anim || !sprite->is_playing()) {
				sprite->play(fungus_anim);
			}
			continue;
		}
		const Vector2 looking_at = body->get(SN_LOOKING_AT);
		const bool flip = looking_at.x < body->get_global_position().x;
		if (flip != sprite->is_flipped_h()) {
			sprite->set_flip_h(flip);
			Node2D *borrowed = Object::cast_to<Node2D>(body->get_node_or_null(NodePath(borrowed_head_name)));
			if (borrowed) {
				Vector2 s = borrowed->get_scale();
				s.x = flip ? -1.0f : 1.0f;
				borrowed->set_scale(s);
			}
		}
		const Vector2 replicated_velocity = body->get(SN_REPLICATED_VELOCITY);
		const StringName &anim = replicated_velocity.length() == 0.0f ? SN_IDLE : SN_WALK;
		if (sprite->get_animation() != anim || !sprite->is_playing()) {
			sprite->play(anim);
		}
		if (!entry.has_footsteps || anim == SN_IDLE || is_walking || bool(body->get(SN_IS_AIMING))) {
			continue;
		}

		body->call(SN_FOOTSTEPS);
	}
	active_count = active;
	live_bots = active;
}

Vector2 BotMotor2D::wall_avoidance(RID p_space, RID p_exclude, Vector2 p_origin, Vector2 p_direction, float p_half_spread, float p_detect, int p_count, uint32_t p_mask, float p_push_strength, float p_max_length) {
	GodotProfileZone("motor wall_avoidance");
	PhysicsDirectSpaceState2D *space = PhysicsServer2D::get_singleton()->space_get_direct_state(p_space);
	ERR_FAIL_NULL_V(space, Vector2());
	PhysicsDirectSpaceState2D::RayParameters params;
	params.from = p_origin;
	params.collision_mask = p_mask;
	params.collide_with_bodies = true;
	params.collide_with_areas = false;
	if (p_exclude.is_valid()) {
		params.exclude.insert(p_exclude);
	}
	Vector2 avoidance;
	const int count = MAX(p_count, 2);
	for (int i = 0; i < count; i++) {
		const float t = float(i) / float(count - 1);
		const float angle = Math::lerp(-p_half_spread, p_half_spread, t);
		params.to = p_origin + p_direction.rotated(angle) * p_detect;
		PhysicsDirectSpaceState2D::RayResult result;
		if (!space->intersect_ray(params, result)) {
			continue;
		}
		const Vector2 diff = p_origin - result.position;
		const float dist = diff.length();
		if (dist > 0.0f) {
			const float k = 1.0f - dist / p_detect;
			avoidance += diff / dist * (k * k) * p_push_strength;
		}
	}
	return avoidance.limit_length(p_max_length);
}

// ---------------------------------------------------------------------------------------------
// Bot brains: BehaviourComposer._process + NaturalMovement.move_to_target.
// Mirrors core/ai/bot/compositions/behaviour_composer.gd and
// core/ai/bot/components/movement/natural_movement.gd - change both together.
// ---------------------------------------------------------------------------------------------

namespace {
const float LOD_MOVEMENT_HZ[3] = { 30.0f, 20.0f, 10.0f };
const float LOD_LOOK_HZ[3] = { 30.0f, 20.0f, 8.0f };
const float LOD_REFRESH_INTERVAL = 0.5f;
const float CROWD_BOTS_PER_STEP = 40.0f;
const float CROWD_MAX_SCALE = 3.0f;
const float CROWD_MIN_MOVEMENT_HZ = 10.0f;
const float CROWD_MAX_LOOK_SCALE = 2.0f;
const float UNSTICK_REROLL_INTERVAL = 0.3f;
const float WALL_PROXIMITY_SUPPRESSION = 2.0f;
const float RAY_HALF_SPREAD = Math::PI / 12.0f;
const float MAX_AVOIDANCE = 0.8f;
const uint32_t WALL_COLLISION_MASK = 0b10100;

float rand_range(float p_from, float p_to) {
	return Math::random(p_from, p_to);
}
} // namespace

void BotMotor2D::register_composer(Node *p_composer) {
	ERR_FAIL_NULL(p_composer);
	const ObjectID id = p_composer->get_instance_id();
	for (const Brain &b : brains) {
		if (b.composer == id) {
			return;
		}
	}
	Brain b;
	b.composer = id;
	brains.push_back(b);
}

void BotMotor2D::reset_composer(Node *p_composer) {
	ERR_FAIL_NULL(p_composer);
	const ObjectID id = p_composer->get_instance_id();
	for (Brain &b : brains) {
		if (b.composer == id) {
			const ObjectID composer = b.composer;
			const bool owned = b.owned;
			b = Brain();
			b.composer = composer;
			b.owned = owned;
			return;
		}
	}
}

void BotMotor2D::_bind_movement(Brain &b, Object *p_movement) {
	b.movement = p_movement ? p_movement->get_instance_id() : ObjectID();
	b.natural = false;
	if (!p_movement) {
		return;
	}
	Ref<Script> script = p_movement->get_script();
	b.natural = script.is_valid() && script->get_global_name() == SN_NATURAL_MOVEMENT;
	if (!b.natural) {
		return;
	}
	b.hesitation_chance = p_movement->get("hesitation_chance");
	b.hesitation_min = p_movement->get("hesitation_min_duration");
	b.hesitation_max = p_movement->get("hesitation_max_duration");
	b.walk_close_distance = p_movement->get("walk_close_distance");
	b.walk_close_probability = p_movement->get("walk_close_probability");
	b.walk_far_distance = p_movement->get("walk_far_distance");
	b.walk_far_probability = p_movement->get("walk_far_probability");
	b.walk_medium_probability = p_movement->get("walk_medium_probability");
	b.strafe_chance = p_movement->get("strafe_chance");
	b.strafe_min = p_movement->get("strafe_min_strength");
	b.strafe_max = p_movement->get("strafe_max_strength");
	b.deviation_min = p_movement->get("deviation_min_strength");
	b.deviation_max = p_movement->get("deviation_max_strength");
	b.arrival_distance_squared = p_movement->get("arrival_distance_squared");
	b.stuck_threshold = p_movement->get("stuck_threshold");
	b.stuck_movement_threshold = p_movement->get("stuck_movement_threshold");
	b.wall_detect_distance = p_movement->get("wall_detect_distance");
	b.wall_push_strength = p_movement->get("wall_push_strength");
	b.idle_wall_detect_distance = p_movement->get("idle_wall_detect_distance");
	b.idle_ray_half_spread = p_movement->get("idle_ray_half_spread");
	b.ray_count = p_movement->get("ray_count");
	b.idle_ray_count = p_movement->get("idle_ray_count");
	b.deterministic = p_movement->get("deterministic");
	// Take over the GDScript component's live state so nothing jumps on handover.
	b.last_position = p_movement->get("_last_position");
	b.stuck_timer = p_movement->get("_stuck_timer");
	b.hesitation_timer = p_movement->get("_hesitation_timer");
	b.walk_change_timer = p_movement->get("_walk_change_timer");
	b.strafe_timer = p_movement->get("_strafe_timer");
	b.deviation_timer = p_movement->get("_deviation_timer");
	b.path_recalc_timer = p_movement->get("_path_recalc_timer");
	b.current_strafe = p_movement->get("_current_strafe");
	b.current_deviation = p_movement->get("_current_deviation");
	b.should_walk = p_movement->get("_should_walk");
	b.last_walk_sent = p_movement->get("_last_walk_sent");
}

void BotMotor2D::_tick_brains(double p_delta) {
	GodotProfileZone("motor _tick_brains");
	if (!is_inside_tree()) {
		return;
	}
	const float delta = p_delta;
	bool frozen = false;
	bool slowmo_bots = true;
	Node *hitstop = get_tree()->get_root()->get_node_or_null(NodePath("HitstopManager"));
	if (hitstop) {
		frozen = hitstop->get(SN_IS_FROZEN);
		slowmo_bots = hitstop->get(SN_ENABLE_SLOWMO_BOTS);
	}
	const float crowd = CLAMP(float(live_bots) / CROWD_BOTS_PER_STEP, 1.0f, CROWD_MAX_SCALE);
	const uint64_t now = OS::get_singleton()->get_ticks_msec();

	for (uint32_t i = 0; i < brains.size();) {
		Brain &b = brains[i];
		Node *composer = ObjectDB::get_instance<Node>(b.composer);
		if (!composer) {
			brains.remove_at_unordered(i);
			continue;
		}
		i++;
		if (!composer->is_inside_tree() || !composer->can_process()) {
			continue;
		}
		if (!b.owned) {
			b.owned = true;
			composer->set_process(false);
		} else if (composer->is_processing()) {
			composer->set_process(false);
		}
		if (frozen && !slowmo_bots) {
			continue;
		}
		Object *character = composer->get(SN_CHARACTER);
		Object *input = composer->get(SN_SCAV_INPUT);
		if (!character || !input) {
			continue;
		}
		Ref<Script> input_script = input->get_script();
		if (input_script.is_valid() && bool(input_script->get(SN_PAUSED))) {
			composer->call(SN_PROCESS, p_delta);
			continue;
		}

		const float flash = composer->get(SN_FLASHBANG_TIMER);
		if (flash > 0.0f) {
			composer->set(SN_FLASHBANG_TIMER, flash - delta);
			composer->set(SN_IS_FLASHBANGED, flash - delta > 0.0f);
		}

		b.lod_refresh_timer -= delta;
		if (b.lod_refresh_timer <= 0.0f) {
			b.lod_refresh_timer = LOD_REFRESH_INTERVAL;
			composer->call(SN_UPDATE_LOD_BAND);
		}
		const int band = CLAMP(int(int64_t(composer->get(SN_LOD_BAND))), 0, 2);

		Object *movement = composer->get(SN_MOVEMENT_COMPONENT);
		if (movement) {
			if (movement->get_instance_id() != b.movement) {
				_bind_movement(b, movement);
			}
			b.movement_accum += delta;
			float hz = LOD_MOVEMENT_HZ[band];
			hz = MIN(hz, MAX(hz / crowd, CROWD_MIN_MOVEMENT_HZ));
			const uint64_t period = uint64_t(1000.0f / hz);
			if (now >= b.movement_next_ms) {
				b.movement_next_ms = now + period;
				if (b.natural) {
					_natural_move(b, composer, movement, b.movement_accum);
				} else {
					movement->call(SN_MOVE_TO_TARGET, b.movement_accum);
				}
				b.movement_accum = 0.0f;
			}
		}

		b.look_accum += delta;
		if (b.look_accum < MIN(crowd, CROWD_MAX_LOOK_SCALE) / LOD_LOOK_HZ[band]) {
			continue;
		}
		composer->call(SN_LOOK_TICK, b.look_accum);
		b.look_accum = 0.0f;
	}
}

void BotMotor2D::_natural_move(Brain &b, Object *p_composer, Object *p_movement, float p_delta) {
	GodotProfileZone("motor _natural_move");
	Node2D *character = Object::cast_to<Node2D>(p_composer->get(SN_CHARACTER).get_validated_object());
	Object *input = p_composer->get(SN_SCAV_INPUT);
	NavigationAgent2D *nav = Object::cast_to<NavigationAgent2D>(p_composer->get(SN_NAVIGATION_AGENT).get_validated_object());
	if (!character || !input || !nav) {
		return;
	}
	auto set_walk = [&](bool p_walking) {
		if (b.last_walk_sent == p_walking) {
			return;
		}
		b.last_walk_sent = p_walking;
		p_movement->set("_last_walk_sent", p_walking);
		input->call(SN_TOGGLE_WALK, p_walking);
	};

	// NaturalMovement._on_flashbanged still runs in GDScript; adopt what it set.
	const float gd_flash = p_movement->get(SN_FLASHBANG_TIMER);
	if (gd_flash > 0.0f) {
		b.flash_timer = gd_flash;
		b.flash_still = p_movement->get(SN_FLASH_STAND_STILL);
		b.flash_direction = p_movement->get(SN_FLASH_DIRECTION);
		b.current_strafe = Vector2();
		b.current_deviation = Vector2();
		p_movement->set(SN_FLASHBANG_TIMER, 0.0f);
	}

	const Vector2 current_position = character->get_global_position();
	const float distance_moved_squared = current_position.distance_squared_to(b.last_position);
	b.last_position = current_position;

	if (b.flash_timer > 0.0f) {
		b.flash_timer -= p_delta;
		input->set(SN_MOVEMENT, b.flash_still ? Vector2() : b.flash_direction);
		set_walk(false);
		return;
	}
	if (nav->is_navigation_finished()) {
		input->set(SN_MOVEMENT, Vector2());
		set_walk(false);
		return;
	}
	const Vector2 target_position = nav->get_next_path_position();
	const Vector2 to_target = target_position - current_position;
	if (to_target.length_squared() < b.arrival_distance_squared) {
		input->set(SN_MOVEMENT, Vector2());
		set_walk(false);
		return;
	}
	const Vector2 base_direction = to_target.normalized();
	const bool has_combat = p_movement->call(SN_HAS_COMBAT_TARGET);
	const bool urgent = p_movement->get(SN_URGENT_MODE);
	const bool berserk = p_movement->get(SN_BERSERK);

	// _update_stuck_detection
	const float threshold_per_frame = b.stuck_movement_threshold * p_delta;
	if (distance_moved_squared < threshold_per_frame * threshold_per_frame) {
		b.stuck_timer += p_delta;
		if (b.stuck_timer > b.stuck_threshold) {
			if (b.deterministic) {
				b.current_strafe = Vector2(0.6f, 0.6f);
				b.strafe_timer = 1.5f;
				b.current_deviation = Vector2();
				b.deviation_timer = 2.0f;
			} else {
				b.current_strafe = Vector2(rand_range(-0.8f, 0.8f), rand_range(-0.8f, 0.8f));
				b.strafe_timer = rand_range(1.0f, 2.0f);
				b.current_deviation = Vector2(rand_range(-0.5f, 0.5f), rand_range(-0.5f, 0.5f));
				b.deviation_timer = rand_range(1.0f, 3.0f);
			}
			// Unsticking re-rolls every 0.3 s; a crowd pinning bots against each other made that a
			// path query per stuck bot every 0.3 s. The repath half is limited to every 2 s.
			const uint64_t stuck_now = OS::get_singleton()->get_ticks_msec();
			if (stuck_now - b.stuck_recalc_ms >= 2000) {
				b.stuck_recalc_ms = stuck_now;
				b.path_recalc_timer = 0.0f;
			}
			b.stuck_timer = b.stuck_threshold - UNSTICK_REROLL_INTERVAL;
		}
	} else {
		b.stuck_timer = 0.0f;
	}

	// _update_hesitation_behavior
	b.hesitation_timer -= p_delta;
	if (b.hesitation_timer <= 0.0f && !has_combat && !urgent && !berserk && !b.deterministic) {
		if (Math::randf() < b.hesitation_chance * p_delta * 60.0f) {
			b.hesitation_timer = rand_range(b.hesitation_min, b.hesitation_max);
		}
	}

	// _update_walk_run_behavior
	b.walk_change_timer -= p_delta;
	if (b.walk_change_timer <= 0.0f) {
		if (b.deterministic) {
			b.should_walk = false;
			b.walk_change_timer = 3.0f;
		} else {
			const float distance_to_target = current_position.distance_to(target_position);
			if (distance_to_target < b.walk_close_distance) {
				b.should_walk = Math::randf() < b.walk_close_probability;
			} else if (distance_to_target > b.walk_far_distance) {
				b.should_walk = Math::randf() < b.walk_far_probability;
			} else {
				b.should_walk = Math::randf() < b.walk_medium_probability;
			}
			b.walk_change_timer = rand_range(1.5f, 5.0f);
		}
	}

	// _update_path_recalculation
	b.path_recalc_timer -= p_delta;
	if (b.path_recalc_timer <= 0.0f) {
		nav->set_target_position(nav->get_target_position());
		b.path_recalc_timer = b.deterministic ? 7.0f : rand_range(4.0f, 10.0f);
	}

	if (b.hesitation_timer > 0.0f && !has_combat && !urgent && !berserk) {
		input->set(SN_MOVEMENT, Vector2());
		set_walk(false);
		return;
	}

	// MovementComponent.wall_avoidance (cached at avoidance_recalc_hz)
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (now >= b.avoidance_next_ms) {
		const float recalc_hz = MAX(1.0f, float(p_movement->get(SN_AVOIDANCE_RECALC_HZ)));
		b.avoidance_next_ms = now + uint64_t(1000.0f / recalc_hz);
		const float half_spread = has_combat ? RAY_HALF_SPREAD : b.idle_ray_half_spread;
		const float detect = has_combat ? b.wall_detect_distance : b.idle_wall_detect_distance;
		const int count = has_combat ? b.ray_count : b.idle_ray_count;
		CollisionObject2D *body = Object::cast_to<CollisionObject2D>(character);
		b.avoidance_cache = wall_avoidance(character->get_world_2d()->get_space(), body ? body->get_rid() : RID(), current_position, base_direction, half_spread, detect, count, WALL_COLLISION_MASK, b.wall_push_strength, MAX_AVOIDANCE);
	}
	const Vector2 avoidance = b.avoidance_cache;
	b.wall_proximity = avoidance.length();

	if (berserk || urgent) {
		const Vector2 charge_unstick = b.stuck_timer >= b.stuck_threshold * 0.5f ? b.current_strafe : Vector2();
		input->set(SN_MOVEMENT, (base_direction + avoidance + charge_unstick).normalized());
		set_walk(false);
		return;
	}

	// _update_strafe_behavior
	b.strafe_timer -= p_delta;
	if (b.strafe_timer <= 0.0f) {
		if (b.deterministic) {
			b.current_strafe = Vector2();
			b.strafe_timer = 2.0f;
		} else if (Math::randf() < b.strafe_chance) {
			const float strength = rand_range(b.strafe_min, b.strafe_max);
			const Vector2 perpendicular(-base_direction.y, base_direction.x);
			const float side = Math::randf() < 0.5f ? 1.0f : -1.0f;
			b.current_strafe = perpendicular * side * strength;
			b.strafe_timer = rand_range(0.8f, 2.5f);
		} else {
			b.current_strafe = Vector2();
			b.strafe_timer = rand_range(1.0f, 4.0f);
		}
	}

	// _update_path_deviation
	b.deviation_timer -= p_delta;
	if (b.deviation_timer <= 0.0f) {
		if (b.deterministic) {
			b.current_deviation = Vector2();
			b.deviation_timer = 2.0f;
		} else {
			const float strength = rand_range(b.deviation_min, b.deviation_max);
			b.current_deviation = Vector2(rand_range(-strength, strength), rand_range(-strength, strength));
			b.deviation_timer = rand_range(0.8f, 4.0f);
		}
	}

	const bool is_unsticking = b.stuck_timer >= b.stuck_threshold * 0.5f;
	const float suppress = is_unsticking ? 1.0f : CLAMP(1.0f - b.wall_proximity * WALL_PROXIMITY_SUPPRESSION, 0.0f, 1.0f);
	input->set(SN_MOVEMENT, (base_direction + b.current_strafe * suppress + b.current_deviation * suppress + avoidance).normalized());
	set_walk(b.should_walk);
}

// ---------------------------------------------------------------------------------------------
// Plain projectiles: Projectile._physics_process / _move / _apply_distance_falloff for shots
// without behaviors. Mirrors core/projectile/projectile.gd - change both together.
// ---------------------------------------------------------------------------------------------

void BotMotor2D::register_projectile(Node *p_projectile, Vector2 p_velocity, bool p_enabled) {
	ERR_FAIL_NULL(p_projectile);
	// Pooled shots are re-activated in place, possibly within one frame: update or drop the
	// existing entry, never add a second one.
	const ObjectID id = p_projectile->get_instance_id();
	int64_t index = -1;
	for (uint32_t i = 0; i < shots.size(); i++) {
		if (shots[i].id == id) {
			index = i;
			break;
		}
	}
	if (!p_enabled) {
		if (index >= 0) {
			shots.remove_at_unordered(index);
		}
		return;
	}
	if (index < 0) {
		shots.push_back(Shot());
		index = shots.size() - 1;
	}
	Shot &s = shots[index];
	s.id = id;
	s.velocity = p_velocity;
	s.spawn = p_projectile->get(SN_SPAWN_POSITION);
	s.max_distance = p_projectile->get(SN_MAX_PROJECTILE_DISTANCE);
	s.base_damage = p_projectile->get(SN_BASE_DAMAGE);
	s.visual_only = p_projectile->get(SN_IS_VISUAL_ONLY);
	s.falloff = p_projectile->get(SN_DAMAGE_FALLOFF);
	s.last_damage = p_projectile->get(SN_DAMAGE);
	p_projectile->set_physics_process(false);
}

void BotMotor2D::_tick_shots(double p_delta) {
	GodotProfileZone("motor _tick_shots");
	if (shots.is_empty()) {
		return;
	}
	if (!dome_script_resolved) {
		dome_script_resolved = true;
		const String path = ScriptServer::get_global_class_path("ChampionDome");
		if (!path.is_empty()) {
			dome_script = ResourceLoader::load(path);
		}
	}
	// A live champion dome can swallow shots mid-flight; its check lives in the script tick.
	bool domes = false;
	if (dome_script.is_valid()) {
		const Array active = dome_script->get(SN_ACTIVE);
		domes = !active.is_empty();
	}
	const float delta = p_delta;
	for (uint32_t i = 0; i < shots.size();) {
		Shot &s = shots[i];
		Node2D *shot = ObjectDB::get_instance<Node2D>(s.id);
		if (!shot) {
			shots.remove_at_unordered(i);
			continue;
		}
		i++;
		// Not in the tree yet (bullets are added deferred) or parked in the pool: wait. A pooled
		// shot is re-registered (or dropped) by its next activate().
		if (!shot->is_inside_tree() || !shot->can_process() || bool(shot->get(SN_IS_DEACTIVATING))) {
			continue;
		}
		const Dictionary chunk_contacts = shot->get(SN_CHUNK_CONTACTS);
		if (domes || !chunk_contacts.is_empty()) {
			shot->call(SN_PHYSICS_PROCESS, p_delta);
			continue;
		}
		const Vector2 pos = shot->get_position() + s.velocity * delta;
		shot->set_position(pos);

		const float dist_sq = shot->get_global_position().distance_squared_to(s.spawn);
		if (dist_sq > s.max_distance * s.max_distance) {
			shot->call(SN_DEACTIVATE);
			continue;
		}
		if (s.visual_only || s.max_distance <= 0.0f) {
			continue;
		}
		Curve *curve = Object::cast_to<Curve>(s.falloff.ptr());
		if (!curve) {
			continue;
		}
		const float normalized = CLAMP(Math::sqrt(dist_sq) / s.max_distance, 0.0f, 1.0f);
		const int damage = int(Math::round(s.base_damage * curve->sample(normalized)));
		if (damage != s.last_damage) {
			s.last_damage = damage;
			shot->set(SN_DAMAGE, damage);
		}
		if (damage <= 0) {
			shot->call(SN_DEACTIVATE);
		}
	}
}


namespace {
const uint32_t LOS_MASK = 0b11001000;
const uint64_t LOS_CACHE_MS = 100;
const float LOS_CACHE_GRID = 8.0f;
const int MAX_SIGHT_CANDIDATES = 4;

bool are_hostile(int a, int b) {
	return a == 0 || b == 0 || a != b;
}
} // namespace

int BotMotor2D::_team_of(Object *p_character) const {
	Object *damageable = p_character->get(SN_DAMAGEABLE);
	if (damageable) {
		return damageable->get(SN_TEAM);
	}
	return p_character->get(SN_TEAM);
}

bool BotMotor2D::_usable_target(Object *p_target) const {
	return p_target && bool(p_target->get(SN_IS_ALIVE)) && !bool(p_target->get(SN_IS_DOWNED)) && !bool(p_target->get_meta(SN_INVISIBLE, false));
}

Object *BotMotor2D::_fallback_target(const Array &p_targeting, int p_my_team) const {
	for (const Variant &v : p_targeting) {
		Object *tc = v.get_validated_object();
		if (!tc) {
			continue;
		}
		Object *current = tc->get(SN_CURRENT_TARGET).get_validated_object();
		if (!current || bool(current->get_meta(SN_INVISIBLE, false)) || !are_hostile(p_my_team, _team_of(current))) {
			continue;
		}
		return current;
	}
	return nullptr;
}

bool BotMotor2D::_has_clear_sight(Node2D *p_character, const Vector2 &p_from, const Vector2 &p_to) {
	GodotProfileZone("motor _has_clear_sight");
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (now - los_cache_started_ms >= LOS_CACHE_MS) {
		los_cache.clear();
		los_cache_started_ms = now;
	}
	const Vector4i key(Math::floor(p_from.x / LOS_CACHE_GRID), Math::floor(p_from.y / LOS_CACHE_GRID), Math::floor(p_to.x / LOS_CACHE_GRID), Math::floor(p_to.y / LOS_CACHE_GRID));
	HashMap<Vector4i, bool>::Iterator E = los_cache.find(key);
	if (E) {
		return E->value;
	}
	if (!p_character->is_inside_tree()) {
		return false;
	}
	PhysicsDirectSpaceState2D *space = PhysicsServer2D::get_singleton()->space_get_direct_state(p_character->get_world_2d()->get_space());
	if (!space) {
		return false;
	}
	PhysicsDirectSpaceState2D::RayParameters params;
	params.from = p_from;
	params.to = p_to;
	params.collision_mask = LOS_MASK;
	params.collide_with_areas = true;
	params.collide_with_bodies = true;
	params.hit_from_inside = false;
	CollisionObject2D *body = Object::cast_to<CollisionObject2D>(p_character);
	if (body) {
		params.exclude.insert(body->get_rid());
	}
	PhysicsDirectSpaceState2D::RayResult result;
	const bool clear = !space->intersect_ray(params, result);
	los_cache.insert(key, clear);
	return clear;
}

bool BotMotor2D::has_clear_sight(Node *p_character, const Vector2 &p_from, const Vector2 &p_to) {
	Node2D *character = Object::cast_to<Node2D>(p_character);
	return character && _has_clear_sight(character, p_from, p_to);
}

Variant BotMotor2D::select_best_target(Node *p_composer) {
	GodotProfileZone("motor select_best_target");
	ERR_FAIL_NULL_V(p_composer, Variant(false));
	Node2D *character = Object::cast_to<Node2D>(p_composer->get(SN_CHARACTER).get_validated_object());
	if (!character) {
		return Variant(false);
	}
	Object *combat = p_composer->get(SN_COMBAT_COMPONENT).get_validated_object();
	bool scav = false;
	if (combat) {
		bool native_ok = false;
		for (Ref<Script> s = combat->get_script(); s.is_valid(); s = s->get_base_script()) {
			const StringName name = s->get_global_name();
			if (name == SN_SCAV_COMBAT || name == SN_COMBAT_COMPONENT_CLASS) {
				scav = name == SN_SCAV_COMBAT;
				native_ok = true;
				break;
			}
			if (s->has_method(SN_PRIORITIZE_TARGET)) {
				break;
			}
		}
		if (!native_ok) {
			return Variant(false);
		}
	}

	const int my_team = _team_of(character);
	const Array targeting = p_composer->get(SN_TARGETING_COMPONENTS);
	LocalVector<Object *> all;
	HashSet<ObjectID> seen;
	for (const Variant &v : targeting) {
		Object *tc = v.get_validated_object();
		if (!tc) {
			continue;
		}
		const Array targets = tc->get(SN_TARGETS);
		for (const Variant &tv : targets) {
			Object *t = tv.get_validated_object();
			if (!t || seen.has(t->get_instance_id())) {
				continue;
			}
			seen.insert(t->get_instance_id());
			if (!_usable_target(t) || !are_hostile(my_team, _team_of(t))) {
				continue;
			}
			all.push_back(t);
		}
	}
	if (all.is_empty()) {
		Object *fallback = _fallback_target(targeting, my_team);
		return fallback ? Variant(fallback) : Variant();
	}
	if (!combat) {
		Object *fallback = _fallback_target(targeting, my_team);
		return fallback ? Variant(fallback) : Variant();
	}

	const Vector2 origin = character->get_global_position();
	struct ByDistance {
		float d;
		Object *t;
		bool operator<(const ByDistance &p_other) const { return d < p_other.d; }
	};
	LocalVector<ByDistance> by_distance;
	for (Object *t : all) {
		Node2D *n = Object::cast_to<Node2D>(t);
		by_distance.push_back({ n ? origin.distance_squared_to(n->get_global_position()) : float(Math::INF), t });
	}
	by_distance.sort();
	Object *closest = nullptr;
	for (uint32_t i = 0; i < MIN(by_distance.size(), uint32_t(MAX_SIGHT_CANDIDATES)); i++) {
		Node2D *n = Object::cast_to<Node2D>(by_distance[i].t);
		if (n && _has_clear_sight(character, origin, n->get_global_position())) {
			closest = by_distance[i].t;
			break;
		}
	}

	if (!scav) {
		return closest ? Variant(closest) : Variant(all[0]);
	}

	Object *last = combat->get(SN_LAST_TARGET).get_validated_object();
	bool last_in_targets = false;
	if (last) {
		for (Object *t : all) {
			if (t == last) {
				last_in_targets = true;
				break;
			}
		}
	}
	if (closest) {
		Node2D *last_node = Object::cast_to<Node2D>(last);
		if (last != closest && last_in_targets && last_node && _has_clear_sight(character, origin, last_node->get_global_position())) {
			const float d_last = origin.distance_to(last_node->get_global_position());
			const float d_new = origin.distance_to(Object::cast_to<Node2D>(closest)->get_global_position());
			if (d_new > d_last * 0.7f) {
				return last;
			}
		}
		return closest;
	}
	if (last_in_targets) {
		return last;
	}
	return by_distance[0].t;
}
