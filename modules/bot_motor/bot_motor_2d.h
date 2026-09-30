#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/2d/node_2d.h"
#include "scene/main/node.h"

// Runs the physics step of every registered host-simulated bot in one native loop: the same
// logic as Character._physics_process / update_movement / update_animation in GDScript, minus
// hundreds of script calls per frame. Bodies it drives have their script physics processing
// turned off; anything that stops being an eligible bot is handed back to its script.
class BotMotor2D : public Node {
	GDCLASS(BotMotor2D, Node);

	struct Entry {
		ObjectID id;
		bool owned = false;
		bool has_footsteps = false;
		bool fungus_anim = false;
	};

	// Created with the motor, not at static-init time (StringName needs the string table).
	StringName SN_IS_BOT = "is_bot";
	StringName SN_IS_ALIVE = "is_alive";
	StringName SN_IDX = "idx";
	StringName SN_INPUT = "input";
	StringName SN_MOVEMENT = "movement";
	StringName SN_CURRENT_MAX_VELOCITY = "current_max_velocity";
	StringName SN_IS_WALKING = "is_walking";
	StringName SN_IS_AIMING = "is_aiming";
	StringName SN_RUN_ACCELERATION = "run_acceleration";
	StringName SN_RUN_DECELERATION = "run_deceleration";
	StringName SN_REPLICATED_VELOCITY = "replicated_velocity";
	StringName SN_REPLICATED_POSITION = "replicated_position";
	StringName SN_LOOKING_AT = "looking_at";
	StringName SN_SPRITE = "sprite";
	StringName SN_IS_FROZEN = "is_frozen";
	StringName SN_ENABLE_SLOWMO_BOTS = "enable_slowmo_bots";
	StringName SN_FOOTSTEPS = "_footsteps";
	StringName SN_IDLE = "idle";
	StringName SN_WALK = "walk";
	StringName SN_CHARACTER = "character";
	StringName SN_SCAV_INPUT = "scav_input";
	StringName SN_NAVIGATION_AGENT = "navigation_agent";
	StringName SN_MOVEMENT_COMPONENT = "movement_component";
	StringName SN_PAUSED = "paused";
	StringName SN_PROCESS = "_process";
	StringName SN_FLASHBANG_TIMER = "_flashbang_timer";
	StringName SN_IS_FLASHBANGED = "is_flashbanged";
	StringName SN_UPDATE_LOD_BAND = "_update_lod_band";
	StringName SN_LOD_BAND = "_lod_band";
	StringName SN_MOVE_TO_TARGET = "move_to_target";
	StringName SN_LOOK_TICK = "_look_tick";
	StringName SN_URGENT_MODE = "urgent_mode";
	StringName SN_BERSERK = "berserk";
	StringName SN_HAS_COMBAT_TARGET = "has_combat_target";
	StringName SN_TOGGLE_WALK = "toggle_walk";
	StringName SN_AVOIDANCE_RECALC_HZ = "avoidance_recalc_hz";
	StringName SN_FLASH_STAND_STILL = "_flashbang_stand_still";
	StringName SN_FLASH_DIRECTION = "_flashbang_direction";
	StringName SN_NATURAL_MOVEMENT = "NaturalMovement";
	StringName SN_SPAWN_POSITION = "spawn_position";
	StringName SN_MAX_PROJECTILE_DISTANCE = "max_projectile_distance";
	StringName SN_BASE_DAMAGE = "base_damage";
	StringName SN_DAMAGE = "damage";
	StringName SN_DAMAGE_FALLOFF = "damage_falloff";
	StringName SN_IS_VISUAL_ONLY = "is_visual_only";
	StringName SN_IS_DEACTIVATING = "is_deactivating";
	StringName SN_CHUNK_CONTACTS = "_chunk_contacts";
	StringName SN_PHYSICS_PROCESS = "_physics_process";
	StringName SN_DEACTIVATE = "deactivate";
	StringName SN_ACTIVE = "_active";
	StringName SN_TARGETING_COMPONENTS = "targeting_components";
	StringName SN_TARGETS = "targets";
	StringName SN_CURRENT_TARGET = "_current_target";
	StringName SN_COMBAT_COMPONENT = "combat_component";
	StringName SN_LAST_TARGET = "last_target";
	StringName SN_IS_DOWNED = "is_downed";
	StringName SN_DAMAGEABLE = "damageable";
	StringName SN_TEAM = "team";
	StringName SN_INVISIBLE = "invisible";
	StringName SN_SHOT_ANIM_UNTIL = "_shot_anim_until";
	StringName SN_ATTACK = "attack";
	StringName SN_SCAV_COMBAT = "ScavCombat";
	StringName SN_COMBAT_COMPONENT_CLASS = "CombatComponent";
	StringName SN_FUNGUS_RED = "FungusRed";
	StringName SN_PRIORITIZE_TARGET = "prioritize_target";

	LocalVector<Entry> bots;

	// Per-bot AI tick (BehaviourComposer._process) and NaturalMovement.move_to_target, native.
	struct Brain {
		ObjectID composer;
		ObjectID movement;
		bool owned = false;
		bool natural = false;
		float lod_refresh_timer = 0.0f;
		uint64_t movement_next_ms = 0;
		float movement_accum = 0.0f;
		float look_accum = 0.0f;
		// NaturalMovement exports (read when the movement component is bound).
		float hesitation_chance = 0.008f, hesitation_min = 0.0f, hesitation_max = 1.5f;
		float walk_close_distance = 400.0f, walk_close_probability = 0.3f, walk_far_distance = 2000.0f, walk_far_probability = 0.15f, walk_medium_probability = 0.5f;
		float strafe_chance = 0.12f, strafe_min = 0.15f, strafe_max = 0.8f, deviation_min = 0.1f, deviation_max = 0.25f;
		float arrival_distance_squared = 32.0f, stuck_threshold = 1.0f, stuck_movement_threshold = 32.0f;
		float wall_detect_distance = 24.0f, wall_push_strength = 1.5f, idle_wall_detect_distance = 40.0f, idle_ray_half_spread = Math::PI * 0.45f;
		int ray_count = 5, idle_ray_count = 7;
		bool deterministic = false;
		// NaturalMovement state.
		Vector2 last_position;
		float wall_proximity = 0.0f, stuck_timer = 0.0f, hesitation_timer = 0.0f, walk_change_timer = 0.0f;
		float strafe_timer = 0.0f, deviation_timer = 0.0f, path_recalc_timer = 5.0f;
		Vector2 current_strafe, current_deviation, flash_direction, avoidance_cache;
		bool should_walk = false, flash_still = false, last_walk_sent = false;
		float flash_timer = 0.0f;
		uint64_t avoidance_next_ms = 0;
		uint64_t stuck_recalc_ms = 0;
	};
	LocalVector<Brain> brains;

	// Plain projectiles (base projectile.gd, no behaviors/steering/acceleration): moved and
	// range/damage-falloff'd natively. Anything unusual falls back to the script's own tick.
	struct Shot {
		ObjectID id;
		Vector2 velocity;
		Vector2 spawn;
		float max_distance = 5000.0f;
		int base_damage = 0;
		int last_damage = -1;
		bool visual_only = false;
		Ref<Resource> falloff;
	};
	LocalVector<Shot> shots;
	Ref<Script> dome_script;
	bool dome_script_resolved = false;
	void _tick_shots(double p_delta);

	HashMap<Vector4i, bool> los_cache;
	uint64_t los_cache_started_ms = 0;
	bool _has_clear_sight(Node2D *p_character, const Vector2 &p_from, const Vector2 &p_to);
	int _team_of(Object *p_character) const;
	bool _usable_target(Object *p_target) const;
	Object *_fallback_target(const Array &p_targeting, int p_my_team) const;
	int live_bots = 0;

	void _bind_movement(Brain &b, Object *p_movement);
	void _tick_brains(double p_delta);
	void _natural_move(Brain &b, Object *p_composer, Object *p_movement, float p_delta);
	String borrowed_head_name = "BorrowedHead";
	int bots_per_step_interval = 20;
	int max_step_interval = 4;
	float max_drift = 6.0;
	float view_margin = 0.5;
	int active_count = 0;

	void _step(double p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void register_bot(Node *p_body);
	void unregister_bot(Node *p_body);
	int get_active_count() const { return active_count; }
	void register_composer(Node *p_composer);
	void register_projectile(Node *p_projectile, Vector2 p_velocity, bool p_enabled);
	Variant select_best_target(Node *p_composer);
	bool has_clear_sight(Node *p_character, const Vector2 &p_from, const Vector2 &p_to);
	void reset_composer(Node *p_composer);

	// Bot wall-avoidance ray fan (MovementComponent._compute_wall_avoidance) without a script
	// Dictionary per ray.
	static Vector2 wall_avoidance(RID p_space, RID p_exclude, Vector2 p_origin, Vector2 p_direction, float p_half_spread, float p_detect, int p_count, uint32_t p_mask, float p_push_strength, float p_max_length);

	BotMotor2D();
};
