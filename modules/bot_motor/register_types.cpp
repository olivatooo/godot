#include "register_types.h"

#include "bot_motor_2d.h"

#include "core/object/class_db.h"

void initialize_bot_motor_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(BotMotor2D);
	}
}

void uninitialize_bot_motor_module(ModuleInitializationLevel p_level) {
}
