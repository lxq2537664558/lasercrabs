#include "types.h"
#include "vi_assert.h"

#include "render/views.h"
#include "render/render.h"
#include "render/skinned_model.h"
#include "data/animator.h"
#include "data/array.h"
#include "data/entity.h"
#include "data/components.h"
#include "data/ragdoll.h"
#include "awk.h"
#include "player.h"
#include "physics.h"
#include "entities.h"
#include "walker.h"
#include "common.h"
#include "render/ui.h"
#include "asset/armature.h"
#include "asset/texture.h"
#include "asset/mesh.h"
#include "asset/shader.h"
#include "asset/lookup.h"
#include "asset/soundbank.h"
#include "asset/Wwise_IDs.h"
#include "asset/level.h"
#include "strings.h"
#include "input.h"
#include "mersenne/mersenne-twister.h"
#include <time.h>
#include "cjson/cJSON.h"
#include "audio.h"
#include "menu.h"
#include "scripts.h"
#include "console.h"
#include "ease.h"
#include "minion.h"
#include "parkour.h"
#include "data/behavior.h"
#include "render/particles.h"
#include "ai_player.h"
#include "usernames.h"
#include "utf8/utf8.h"

#if DEBUG
	#define DEBUG_NAV_MESH 0
	#define DEBUG_AI_PATH 0
	#define DEBUG_AWK_NAV_MESH 0
	#define DEBUG_AWK_AI_PATH 0
	#define DEBUG_PHYSICS 0
#endif

#include "game.h"

namespace VI
{

b8 Game::quit = false;
GameTime Game::time = GameTime();
GameTime Game::real_time = GameTime();

AssetID Game::scheduled_load_level = AssetNull;
Game::Mode Game::scheduled_mode = Game::Mode::Pvp;
Game::Save Game::save;
Game::Level Game::level;
Game::State Game::state;
Vec2 Game::cursor(200, 200);
b8 Game::cursor_updated = false;

Game::State::State()
	: mode(Game::Mode::Special),
	local_player_config{ AI::Team::A, AI::Team::None, AI::Team::None, AI::Team::None },
	third_person(),
	local_multiplayer(),
	local_multiplayer_offset(),
	time_scale(1.0f),
	level(AssetNull)
{
}

void Game::State::reset()
{
	AssetID l = level;
	*this = State();
	level = l;
}

const s32 Game::levels[] =
{
	Asset::Level::tutorial,
	Asset::Level::level1,
	Asset::Level::level2,
	AssetNull,
};

b8 Game::Level::has_feature(Game::FeatureLevel f) const
{
	return (s32)feature_level >= (s32)f;
}

Array<UpdateFunction> Game::updates;
Array<DrawFunction> Game::draws;
Array<CleanupFunction> Game::cleanups;

b8 Game::init(LoopSync* sync)
{
	state.local_player_config[0] = AI::Team::A;
	state.mode = Mode::Special;
	state.time_scale = 1.0f;

	if (!Audio::init())
		return false;

	if (!Loader::soundbank_permanent(Asset::Soundbank::Init))
		return false;
	if (!Loader::soundbank_permanent(Asset::Soundbank::SOUNDBANK))
		return false;

	// strings
	{
		const char* language_file = "language.txt";
		cJSON* json_language = Json::load(language_file);
		const char* language = Json::get_string(json_language, "language", "en");
		char ui_string_file[255];
		sprintf(ui_string_file, "assets/str/ui_%s.json", language);
		char dialogue_string_file[255];
		sprintf(dialogue_string_file, "assets/str/dialogue_%s.json", language);
		char misc_file[255];
		sprintf(misc_file, "assets/str/misc_%s.json", language);
		char notes_file[255];
		sprintf(notes_file, "assets/str/notes_%s.json", language);
		Json::json_free(json_language);

		// UI
		{
			cJSON* json = Json::load(ui_string_file);
			for (s32 i = 0; i < Asset::String::count; i++)
			{
				const char* name = AssetLookup::String::names[i];
				cJSON* value = cJSON_GetObjectItem(json, name);
				strings_set(i, value->valuestring);
			}
		}

		// dialogue strings
		{
			cJSON* json = Json::load(dialogue_string_file);
			cJSON* element = json->child;
			while (element)
			{
				strings_add_dynamic(element->string, element->valuestring);
				element = element->next;
			}
		}

		// misc strings
		{
			cJSON* json = Json::load(misc_file);
			cJSON* element = json->child;
			while (element)
			{
				strings_add_dynamic(element->string, element->valuestring);
				element = element->next;
			}
		}

		// notes
		{
			cJSON* json = Json::load(notes_file);
			cJSON* element = json->child;
			while (element)
			{
				strings_add_dynamic(element->string, element->valuestring);
				element = element->next;
			}
		}

		Input::load_strings(); // loads localized strings for input bindings

		// don't free the JSON objects; we'll read strings directly from them
	}

	Penelope::global_init();

	World::init();
	for (s32 i = 0; i < ParticleSystem::all.length; i++)
		ParticleSystem::all[i]->init(sync);

	UI::init(sync);

	Console::init();

	Menu::init();

	return true;
}

void Game::update(const Update& update_in)
{
	real_time = update_in.time;
	time.delta = update_in.time.delta * state.time_scale;
	time.total += time.delta;

	cursor_updated = false;

	Update u = update_in;
	u.time = time;

	Menu::update(u);

	if (scheduled_load_level != AssetNull)
		load_level(u, scheduled_load_level, scheduled_mode);

	AI::update(u);

	Physics::sync_dynamic();

	for (auto i = Ragdoll::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = Animator::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = Mover::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);

	LerpTo<Vec3>::update_active(u);
	Delay::update_active(u);

	Physics::sync_static();

	AI::update(u);

	Team::update_all(u);
	for (auto i = PlayerManager::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	LocalPlayer::update_all(u);
	for (auto i = Walker::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = Awk::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = NoclipControl::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = MinionAI::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = PlayerTrigger::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = AIPlayerControl::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = MinionCommon::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = Parkour::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	Sensor::update_all(u);
	ControlPoint::update_all(u);
	for (auto i = Shockwave::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = Projectile::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = Tile::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);
	for (auto i = Teleportee::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);

	for (s32 i = 0; i < updates.length; i++)
		(*updates[i])(u);

	for (auto i = PlayerCommon::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);

	for (auto i = LocalPlayerControl::list.iterator(); !i.is_last(); i.next())
		i.item()->update(u);

	Console::update(u);

	Audio::update();

	World::flush();
}

void Game::term()
{
	Audio::term();
}

void Game::draw_opaque(const RenderParams& render_params)
{
	View::draw_opaque(render_params);
	Rope::draw_opaque(render_params);
	SkinnedModel::draw_opaque(render_params);
}

void Game::draw_alpha(const RenderParams& render_params)
{
	Skybox::draw_alpha(render_params, level.skybox);
	SkyDecal::draw_alpha(render_params, level.skybox);
	SkyPattern::draw_alpha(render_params);
	SkinnedModel::draw_alpha(render_params);

#if DEBUG_NAV_MESH
	AI::debug_draw_nav_mesh(render_params);
#endif

#if DEBUG_AWK_NAV_MESH
	AI::debug_draw_awk_nav_mesh(render_params);
#endif

#if DEBUG_AI_PATH
	{
		UIText text;
		for (auto i = MinionAI::list.iterator(); !i.is_last(); i.next())
		{
			MinionAI* minion = i.item();
			text.color = Team::ui_colors[(s32)minion->get<AIAgent>()->team];
			for (s32 j = minion->path_index; j < minion->path.length; j++)
			{
				Vec2 p;
				if (UI::project(render_params, minion->path[j], &p))
				{
					text.text("%d", j);
					text.draw(render_params, p);
				}
			}
		}
	}
#endif

#if DEBUG_AWK_AI_PATH
	{
		UIText text;
		for (auto i = AIPlayerControl::list.iterator(); !i.is_last(); i.next())
		{
			AIPlayerControl* ai = i.item();
			text.color = Team::ui_color_enemy;
			for (s32 j = 0; j < ai->path.length; j++)
			{
				Vec2 p;
				if (UI::project(render_params, ai->path[j], &p))
				{
					text.text("%d", j);
					text.draw(render_params, p);
				}
			}
		}
	}
#endif

#if DEBUG_PHYSICS
	{
		RenderSync* sync = render_params.sync;

		sync->write(RenderOp::FillMode);
		sync->write(RenderFillMode::Line);

		Loader::shader_permanent(Asset::Shader::flat);

		sync->write(RenderOp::Shader);
		sync->write(Asset::Shader::flat);
		sync->write(render_params.technique);

		for (auto i = RigidBody::list.iterator(); !i.is_last(); i.next())
		{
			RigidBody* body = i.item();
			btTransform transform = body->btBody->getWorldTransform();

			Vec3 radius;
			Vec4 color;
			AssetID mesh_id;
			switch (body->type)
			{
				case RigidBody::Type::Box:
					mesh_id = Asset::Mesh::cube;
					radius = body->size;
					color = Vec4(1, 0, 0, 1);
					break;
				case RigidBody::Type::Sphere:
					mesh_id = Asset::Mesh::sphere;
					radius = body->size;
					color = Vec4(1, 0, 0, 1);
					break;
				case RigidBody::Type::CapsuleX:
					// capsules: size.x = radius, size.y = height
					mesh_id = Asset::Mesh::cube;
					radius = Vec3(body->size.y * 0.5f, body->size.x, body->size.x);
					color = Vec4(0, 1, 0, 1);
					break;
				case RigidBody::Type::CapsuleY:
					mesh_id = Asset::Mesh::cube;
					radius = Vec3(body->size.x, body->size.y * 0.5f, body->size.x);
					color = Vec4(0, 1, 0, 1);
					break;
				case RigidBody::Type::CapsuleZ:
					mesh_id = Asset::Mesh::cube;
					radius = Vec3(body->size.x, body->size.x, body->size.y * 0.5f);
					color = Vec4(0, 1, 0, 1);
					break;
				default:
					continue;
			}

			if (!render_params.camera->visible_sphere(transform.getOrigin(), vi_max(radius.x, vi_max(radius.y, radius.z))))
				continue;

			Loader::mesh_permanent(mesh_id);

			Mat4 m;
			m.make_transform(transform.getOrigin(), radius, transform.getRotation());
			Mat4 mvp = m * render_params.view_projection;

			sync->write(RenderOp::Uniform);
			sync->write(Asset::Uniform::mvp);
			sync->write(RenderDataType::Mat4);
			sync->write<s32>(1);
			sync->write<Mat4>(mvp);

			sync->write(RenderOp::Uniform);
			sync->write(Asset::Uniform::diffuse_color);
			sync->write(RenderDataType::Vec4);
			sync->write<s32>(1);
			sync->write<Vec4>(color);

			sync->write(RenderOp::Mesh);
			sync->write(RenderPrimitiveMode::Triangles);
			sync->write(mesh_id);
		}
		sync->write(RenderOp::FillMode);
		sync->write(RenderFillMode::Fill);
	}
#endif

	View::draw_alpha(render_params);

	Tile::draw_alpha(render_params);

	for (s32 i = 0; i < ParticleSystem::all.length; i++)
		ParticleSystem::all[i]->draw(render_params);

	for (auto i = LocalPlayerControl::list.iterator(); !i.is_last(); i.next())
		i.item()->draw_alpha(render_params);
	for (auto i = LocalPlayer::list.iterator(); !i.is_last(); i.next())
		i.item()->draw_alpha(render_params);

	Menu::draw(render_params);

	for (s32 i = 0; i < draws.length; i++)
		(*draws[i])(render_params);

	Console::draw(render_params);
}

void Game::draw_cursor(const RenderParams& params)
{
	UI::mesh(params, Asset::Mesh::cursor, Game::cursor + Vec2(-2, 4), Vec2(24) * UI::scale, UI::background_color);
	UI::mesh(params, Asset::Mesh::cursor, Game::cursor, Vec2(18) * UI::scale, UI::default_color);
}

void Game::update_cursor(const Update& u)
{
	if (!cursor_updated)
	{
		cursor_updated = true;
		cursor.x = LMath::clampf(cursor.x + u.input->cursor_x, 0.0f, u.input->width);
		cursor.y = LMath::clampf(cursor.y - u.input->cursor_y, 0.0f, u.input->height);
	}
}

void Game::draw_additive(const RenderParams& render_params)
{
	View::draw_additive(render_params);
	SkinnedModel::draw_additive(render_params);
}

void Game::execute(const Update& u, const char* cmd)
{
	if (utf8cmp(cmd, "third_person") == 0)
	{
		state.third_person = !state.third_person;
	}
	else if (utf8cmp(cmd, "noclip") == 0)
	{
		Vec3 pos = Vec3::zero;
		Quat quat = Quat::identity;
		if (LocalPlayerControl::list.count() > 0)
		{
			auto players = LocalPlayerControl::list.iterator();
			LocalPlayerControl* p = players.item();
			pos = p->get<Transform>()->absolute_pos();
			quat = p->camera->rot;

			while (!players.is_last())
			{
				World::remove(players.item()->entity());
				players.next();
			}

			Noclip* noclip = World::create<Noclip>();
			noclip->get<Transform>()->pos = pos;
		}
		else if (NoclipControl::list.count() > 0)
		{
			NoclipControl* noclip = NoclipControl::list.iterator().item();
			pos = noclip->get<Transform>()->absolute_pos();
			quat = noclip->camera->rot;
			World::remove(noclip->entity());
		}
	}
	else if (strstr(cmd, "timescale ") == cmd)
	{
		const char* delimiter = strchr(cmd, ' ');
		if (delimiter)
		{
			const char* number_string = delimiter + 1;
			char* end;
			r32 value = std::strtod(number_string, &end);
			if (*end == '\0')
				state.time_scale = value;
		}
	}
	else if (strstr(cmd, "credits ") == cmd)
	{
		if (PlayerManager::list.count() > 0)
		{
			const char* delimiter = strchr(cmd, ' ');
			if (delimiter)
			{
				const char* number_string = delimiter + 1;
				char* end;
				s32 value = (s32)std::strtol(number_string, &end, 10);
				if (*end == '\0')
				{
					for (auto i = PlayerManager::list.iterator(); !i.is_last(); i.next())
						i.item()->credits += value;
				}
			}
		}
	}
	else if (strstr(cmd, "loadp ") == cmd)
	{
		// parkour mode
		const char* delimiter = strchr(cmd, ' ');
		if (delimiter)
		{
			const char* level_name = delimiter + 1;
			AssetID level = Loader::find(level_name, AssetLookup::Level::names);
			if (level != AssetNull)
			{
				Game::save = Game::Save();
				Game::state.reset();
				Menu::transition(level, Game::Mode::Parkour);
			}
		}
	}
	else if (strstr(cmd, "loadai ") == cmd)
	{
		// AI test
		const char* delimiter = strchr(cmd, ' ');
		if (delimiter)
		{
			const char* level_name = delimiter + 1;
			AssetID level = Loader::find(level_name, AssetLookup::Level::names);
			if (level != AssetNull)
			{
				Game::save = Game::Save();
				Game::state.reset();
				Game::load_level(u, level, Game::Mode::Pvp, true);
			}
		}
	}
	else if (strstr(cmd, "load ") == cmd)
	{
		// pvp mode
		const char* delimiter = strchr(cmd, ' ');
		if (delimiter)
		{
			const char* level_name = delimiter + 1;
			AssetID level = Loader::find(level_name, AssetLookup::Level::names);
			if (level != AssetNull)
			{
				Game::save = Game::Save();
				Game::state.reset();
				Menu::transition(level, Game::Mode::Pvp);
			}
		}
	}
	else if (strstr(cmd, "loads ") == cmd)
	{
		// special mode
		const char* delimiter = strchr(cmd, ' ');
		if (delimiter)
		{
			const char* level_name = delimiter + 1;
			AssetID level = Loader::find(level_name, AssetLookup::Level::names);
			if (level != AssetNull)
			{
				Game::save = Game::Save();
				Game::state.reset();
				Menu::transition(level, Game::Mode::Special);
			}
		}
	}
}

void Game::schedule_load_level(AssetID level_id, Mode m)
{
	scheduled_load_level = level_id;
	scheduled_mode = m;
}

void Game::unload_level()
{
	for (auto i = Entity::list.iterator(); !i.is_last(); i.next())
		World::remove(i.item());

	World::remove_buffer.length = 0; // any deferred requests to remove entities should be ignored; they're all gone

	for (auto i = LocalPlayer::list.iterator(); !i.is_last(); i.next())
		LocalPlayer::list.remove(i.index);
	for (auto i = AIPlayer::list.iterator(); !i.is_last(); i.next())
		AIPlayer::list.remove(i.index);
	for (auto i = PlayerManager::list.iterator(); !i.is_last(); i.next())
		PlayerManager::list.remove(i.index);
	Team::list.length = 0;

	for (s32 i = 0; i < ParticleSystem::all.length; i++)
		ParticleSystem::all[i]->clear();

	Loader::transients_free();
	updates.length = 0;
	draws.length = 0;
	for (s32 i = 0; i < cleanups.length; i++)
		(*cleanups[i])();
	cleanups.length = 0;

	level.skybox.far_plane = 1.0f;
	level.skybox.fog_start = level.skybox.far_plane * 0.25f;
	level.skybox.color = Vec3::zero;
	level.skybox.ambient_color = Vec3::zero;
	level.skybox.zenith_color = Vec3::zero;
	level.skybox.shader = AssetNull;
	level.skybox.texture = AssetNull;
	level.skybox.mesh = AssetNull;
	level.skybox.player_light = Vec3::zero;

	Audio::post_global_event(AK::EVENTS::STOP_ALL);
	state.level = AssetNull;
	Menu::clear();

	for (s32 i = 0; i < Camera::max_cameras; i++)
	{
		if (Camera::all[i].active)
			Camera::all[i].remove();
	}
}

Entity* EntityFinder::find(const char* name) const
{
	for (s32 j = 0; j < map.length; j++)
	{
		if (utf8cmp(map[j].name, name) == 0)
			return map[j].entity.ref();
	}
	return nullptr;
}

void EntityFinder::add(const char* name, Entity* e)
{
	NameEntry* entry = map.add();
	entry->name = name;
	entry->entity = e;
}

template<typename T>
struct LevelLink
{
	Ref<T>* ref;
	const char* target_name;
};

struct MoverEntry
{
	Mover* mover;
	const char* object_name;
	const char* end_name;
	r32 speed;
};

struct RopeEntry
{
	Vec3 pos;
	Quat rot;
	r32 max_distance;
	r32 slack;
};

AI::Team team_lookup(AI::Team* table, s32 i)
{
	return table[vi_max(0, vi_min((s32)AI::Team::count - 1, i))];
}

void Game::load_level(const Update& u, AssetID l, Mode m, b8 ai_test)
{
	time.total = 0.0f;

	state.mode = m;

	scheduled_load_level = AssetNull;

	unload_level();

	Audio::post_global_event(AK::EVENTS::PLAY_START_SESSION);

	state.level = l;

	Physics::btWorld->setGravity(btVector3(0, -12.0f, 0));

	Array<Transform*> transforms;

	Array<RopeEntry> ropes;

	Array<Script*> scripts;

	Array<LevelLink<Entity>> links;
	Array<LevelLink<Transform>> transform_links;
	Array<MoverEntry> mover_links;

	EntityFinder finder;
	
	cJSON* json = Loader::level(state.level, m == Mode::Pvp);

	const Vec3 pvp_accessible(0.6f);
	const Vec3 pvp_inaccessible(0.0f);
	const Vec3 pvp_sky(0.0f);
	const Vec3 pvp_ambient(0.15f);
	const Vec3 pvp_zenith(0.0f);
	const Vec3 pvp_player_light(1.0f);

	AI::Team teams[(s32)AI::Team::count];

	cJSON* element = json->child;
	while (element)
	{
		Entity* entity = nullptr;

		Vec3 absolute_pos;
		Quat absolute_rot;
		s32 parent = cJSON_GetObjectItem(element, "parent")->valueint;

		{
			Vec3 pos = Json::get_vec3(element, "pos");
			Quat rot = Json::get_quat(element, "rot");
			absolute_pos = pos;
			absolute_rot = rot;

			if (parent != -1)
				transforms[parent]->to_world(&absolute_pos, &absolute_rot);
		}

		b8 alpha = (b8)Json::get_s32(element, "alpha");
		b8 additive = (b8)Json::get_s32(element, "additive");
		s32 order = Json::get_s32(element, "order");

		if (cJSON_GetObjectItem(element, "StaticGeom"))
		{
			cJSON* meshes = cJSON_GetObjectItem(element, "meshes");
			cJSON* json_mesh = meshes->child;

			while (json_mesh)
			{
				char* mesh_ref = json_mesh->valuestring;

				Entity* m;

				s32 mesh_id = Loader::find(mesh_ref, AssetLookup::Mesh::names);
				Mesh* mesh = Loader::mesh(mesh_id);
				vi_assert(mesh_id != AssetNull);
				if (mesh->color.w < 0.5f)
				{
					// inaccessible
					m = World::alloc<StaticGeom>(mesh_id, absolute_pos, absolute_rot, CollisionInaccessible, CollisionInaccessibleMask);
					Vec4 color = Loader::mesh(mesh_id)->color;
					if (state.mode != Mode::Parkour) // override colors
						color.xyz(pvp_inaccessible);
					color.w = MATERIAL_NO_OVERRIDE; // special G-buffer index for inaccessible materials
					m->get<View>()->color = color;
				}
				else
				{
					// accessible
					m = World::alloc<StaticGeom>(mesh_id, absolute_pos, absolute_rot);

					Vec4 color = Loader::mesh(mesh_id)->color;
					if (state.mode != Mode::Parkour) // override colors
						color.xyz(pvp_accessible);
					m->get<View>()->color = color;
				}

				if (entity)
				{
					World::awake(m);
					m->get<Transform>()->reparent(entity->get<Transform>());
				}
				else
					entity = m;

				if (alpha || additive)
					m->get<View>()->shader = Asset::Shader::flat;
				if (alpha)
					m->get<View>()->alpha();
				if (additive)
					m->get<View>()->additive();

				json_mesh = json_mesh->next;
			}
		}
		else if (cJSON_GetObjectItem(element, "Rope"))
		{
			RopeEntry* rope = ropes.add();
			rope->pos = absolute_pos;
			rope->rot = absolute_rot;
			rope->slack = Json::get_r32(element, "slack");
			rope->max_distance = Json::get_r32(element, "max_distance", 100.0f);
		}
		else if (cJSON_GetObjectItem(element, "Minion"))
		{
			if (state.mode == Game::Mode::Pvp)
			{
				AI::Team team = (AI::Team)Json::get_s32(element, "team");
				entity = World::alloc<Minion>(absolute_pos, absolute_rot, team);
				s32 health = Json::get_s32(element, "health");
				if (health)
					entity->get<Health>()->hp = health;
			}
		}
		else if (cJSON_GetObjectItem(element, "PlayerSpawn"))
		{
			AI::Team team = (AI::Team)Json::get_s32(element, "team");

			entity = World::alloc<PlayerSpawn>(team);

			if (Team::list.length > 0)
				Team::list[(s32)team].player_spawn = entity->get<Transform>();
		}
		else if (cJSON_GetObjectItem(element, "PlayerTrigger"))
		{
			entity = World::alloc<Empty>();
			PlayerTrigger* trigger = entity->create<PlayerTrigger>();
			trigger->radius = Json::get_r32(element, "scale", 1.0f) * 0.5f;
		}
		else if (cJSON_GetObjectItem(element, "Mover"))
		{
			entity = World::alloc<MoverEntity>((b8)Json::get_s32(element, "reversed"), (b8)Json::get_s32(element, "translation", 1), (b8)Json::get_s32(element, "rotation", 1));
			cJSON* entity_link = cJSON_GetObjectItem(element, "links")->child;
			if (entity_link)
			{
				MoverEntry* entry = mover_links.add();
				entry->mover = entity->get<Mover>();
				entry->object_name = entity_link->valuestring;
				entry->end_name = entity_link->next->valuestring;
				entry->speed = Json::get_r32(element, "speed", 1.0f);
			}
			Mover* mover = entity->get<Mover>();
			mover->ease = (Ease::Type)Json::get_enum(element, "ease", Ease::type_names);
		}
		else if (cJSON_GetObjectItem(element, "World"))
		{
			// World is guaranteed to be the first element in the entity list

			if (state.mode == Mode::Parkour)
			{
				const char* entry_point_str = Json::get_string(element, "dialogue");
				AssetID entry_point = strings_get(entry_point_str);
				Penelope::init(entry_point);
			}

			level.feature_level = (FeatureLevel)Json::get_s32(element, "feature_level", (s32)FeatureLevel::All);

			{
				b8 lock_teams = Json::get_s32(element, "lock_teams");
				// shuffle teams
				// if we're in local multiplayer mode, rotate the teams by a set amount
				// local multiplayer games rotate through the possible team configurations on each map before moving to the next map
				s32 offset;
				if (lock_teams)
					offset = 0;
				else
					offset = state.local_multiplayer ? state.local_multiplayer_offset : mersenne::rand() % (s32)AI::Team::count;

				for (s32 i = 0; i < (s32)AI::Team::count; i++)
					teams[i] = (AI::Team)((offset + i) % (s32)AI::Team::count);
			}

			level.skybox.far_plane = Json::get_r32(element, "far_plane", 100.0f);
			level.skybox.texture = Loader::find(Json::get_string(element, "skybox_texture"), AssetLookup::Texture::names);
			level.skybox.shader = Asset::Shader::skybox;
			level.skybox.mesh = Asset::Mesh::skybox;
			if (state.mode == Mode::Pvp)
			{
				// override colors
				level.skybox.fog_start = level.skybox.far_plane;
				level.skybox.color = pvp_sky;
				level.skybox.ambient_color = pvp_ambient;
				level.skybox.zenith_color = pvp_zenith;
				level.skybox.player_light = pvp_player_light;
			}
			else
			{
				level.skybox.fog_start = level.skybox.far_plane * 0.25f;
				level.skybox.color = Json::get_vec3(element, "skybox_color");
				level.skybox.ambient_color = Json::get_vec3(element, "ambient_color");
				level.skybox.zenith_color = Json::get_vec3(element, "zenith_color");
				level.skybox.player_light = Vec3::zero;
			}

			level.min_y = Json::get_r32(element, "min_y", -20.0f);

			// initialize teams
			if (m != Mode::Special)
			{
				for (s32 i = 0; i < (s32)AI::Team::count; i++)
				{
					Team* team = Team::list.add();
					new (team) Team();
				}

				for (s32 i = 0; i < MAX_GAMEPADS; i++)
				{
					if (state.local_player_config[i] != AI::Team::None)
					{
						AI::Team team = team_lookup(teams, (s32)state.local_player_config[i]);

						PlayerManager* manager = PlayerManager::list.add();
						new (manager) PlayerManager(&Team::list[(s32)team]);

						if (ai_test)
						{
							AIPlayer* player = AIPlayer::list.add();
							new (player) AIPlayer(manager);
						}
						else
						{
							LocalPlayer* player = LocalPlayer::list.add();
							new (player) LocalPlayer(manager, i);
						}
					}
				}
				if (state.mode == Mode::Pvp)
					Audio::post_global_event(AK::EVENTS::PLAY_MUSIC_01);
			}
		}
		else if (cJSON_GetObjectItem(element, "PointLight"))
		{
			entity = World::alloc<Empty>();
			PointLight* light = entity->create<PointLight>();
			light->color = Json::get_vec3(element, "color");
			light->radius = Json::get_r32(element, "radius");
		}
		else if (cJSON_GetObjectItem(element, "DirectionalLight"))
		{
			entity = World::alloc<Empty>();
			if (state.mode == Mode::Parkour)
			{
				DirectionalLight* light = entity->create<DirectionalLight>();
				light->color = Json::get_vec3(element, "color");
				light->shadowed = Json::get_s32(element, "shadowed");
			}
		}
		else if (cJSON_GetObjectItem(element, "AIPlayer"))
		{
			// only add an AI player if we are in online pvp mode
			if (state.mode == Mode::Pvp && !state.local_multiplayer)
			{
				AI::Team team = team_lookup(teams, Json::get_s32(element, "team", (s32)AI::Team::B));

				PlayerManager* manager = PlayerManager::list.add();
				new (manager) PlayerManager(&Team::list[(s32)team]);

				utf8cpy(manager->username, Usernames::all[mersenne::rand_u32() % Usernames::count]);

				AIPlayer* player = AIPlayer::list.add();
				new (player) AIPlayer(manager);
			}
		}
		else if (cJSON_GetObjectItem(element, "HealthPickup"))
		{
			if (level.has_feature(FeatureLevel::HealthPickups))
			{
				entity = World::alloc<HealthPickupEntity>();

				RopeEntry* rope = ropes.add();
				rope->pos = absolute_pos + Vec3(0, 1, 0);
				rope->rot = Quat::identity;
				rope->slack = 0.0f;
				rope->max_distance = 100.0f;
			}
		}
		else if (cJSON_GetObjectItem(element, "Terminal"))
		{
			if (state.mode == Mode::Parkour)
			{
				entity = World::alloc<Terminal>();
				absolute_pos.y += TERMINAL_HEIGHT * 0.5f;
				Penelope::add_terminal(entity);
			}
		}
		else if (cJSON_GetObjectItem(element, "SensorInterestPoint"))
		{
			if (state.mode == Mode::Pvp)
			{
				entity = World::alloc<Empty>();
				entity->create<SensorInterestPoint>();
			}
		}
		else if (cJSON_GetObjectItem(element, "SkyDecal"))
		{
			entity = World::alloc<Empty>();

			if (state.mode == Mode::Parkour)
			{
				SkyDecal* decal = entity->create<SkyDecal>();
				decal->color = Vec4(Json::get_r32(element, "r", 1.0f), Json::get_r32(element, "g", 1.0f), Json::get_r32(element, "b", 1.0f), Json::get_r32(element, "a", 1.0f));
				decal->scale = Json::get_r32(element, "scale", 1.0f);
				decal->texture = Loader::find(Json::get_string(element, "SkyDecal"), AssetLookup::Texture::names);
			}
		}
		else if (cJSON_GetObjectItem(element, "ControlPoint"))
		{
			if (level.has_feature(FeatureLevel::Abilities))
				entity = World::alloc<ControlPointEntity>();
		}
		else if (cJSON_GetObjectItem(element, "Script"))
		{
			const char* name = Json::get_string(element, "Script");
			Script* script = Script::find(name);
			vi_assert(script);
			scripts.add(script);
		}
		else if (cJSON_GetObjectItem(element, "DataFragment"))
		{
			if (state.mode == Mode::Parkour)
			{
				AssetID note = strings_get(Json::get_string(element, "DataFragment"));
				entity = World::alloc<DataFragmentEntity>(absolute_pos, absolute_rot, note);
			}
		}
		else if (cJSON_GetObjectItem(element, "Prop"))
		{
			const char* name = Json::get_string(element, "Prop");

			b8 alpha = (b8)Json::get_s32(element, "alpha");
			b8 additive = (b8)Json::get_s32(element, "additive");
			s32 order = Json::get_s32(element, "order");
			const char* armature = Json::get_string(element, "armature");
			const char* animation = Json::get_string(element, "animation");

			if (name)
			{
				entity = World::alloc<Prop>(Loader::find(name, AssetLookup::Mesh::names), Loader::find(armature, AssetLookup::Armature::names), Loader::find(animation, AssetLookup::Animation::names));
				// todo: clean this up
				if (entity->has<View>())
				{
					if (alpha || additive)
						entity->get<View>()->shader = Asset::Shader::flat;
					else if (state.mode == Mode::Pvp)
					{
						if (entity->get<View>()->color.w < 0.5f)
							entity->get<View>()->color = Vec4(pvp_inaccessible, MATERIAL_NO_OVERRIDE);
						else
							entity->get<View>()->color.xyz(pvp_accessible);
					}
					if (alpha)
						entity->get<View>()->alpha();
					if (additive)
						entity->get<View>()->additive();
				}
				else
				{
					// SkinnedModel
					if (alpha || additive)
						entity->get<SkinnedModel>()->shader = Asset::Shader::flat;
					else if (state.mode == Mode::Pvp)
					{
						if (entity->get<SkinnedModel>()->color.w < 0.5f)
							entity->get<SkinnedModel>()->color = Vec4(pvp_inaccessible, MATERIAL_NO_OVERRIDE);
						else
							entity->get<SkinnedModel>()->color.xyz(pvp_accessible);
					}
					if (alpha)
						entity->get<SkinnedModel>()->alpha();
					if (additive)
						entity->get<SkinnedModel>()->additive();
				}
			}

			cJSON* meshes = cJSON_GetObjectItem(element, "meshes");

			vi_assert(name || meshes);

			if (meshes)
			{
				cJSON* mesh = meshes->child;
				while (mesh)
				{
					char* mesh_ref = mesh->valuestring;

					AssetID mesh_id = Loader::find(mesh_ref, AssetLookup::Mesh::names);

					Entity* m = World::alloc<Prop>(mesh_id);
					if (entity)
					{
						World::awake(m);
						m->get<Transform>()->reparent(entity->get<Transform>());
					}
					else
						entity = m;

					if (alpha || additive)
						m->get<View>()->shader = Asset::Shader::flat;
					else if (state.mode == Mode::Pvp)
					{
						if (entity->get<View>()->color.w < 0.5f)
							entity->get<View>()->color = Vec4(pvp_inaccessible, MATERIAL_NO_OVERRIDE);
						else
							entity->get<View>()->color.xyz(pvp_accessible);
					}

					if (alpha)
						m->get<View>()->alpha();
					if (additive)
						m->get<View>()->additive();

					mesh = mesh->next;
				}
			}
		}
		else
			entity = World::alloc<Empty>();

		if (entity && entity->has<Transform>())
		{
			Transform* transform = entity->get<Transform>();

			if (parent != -1)
				transform->parent = transforms[parent];

			transform->absolute(absolute_pos, absolute_rot);

			transforms.add(transform);
		}
		else
			transforms.add(nullptr);

		if (entity)
			finder.add(Json::get_string(element, "name"), entity);

		element = element->next;
	}

	for (s32 i = 0; i < links.length; i++)
	{
		LevelLink<Entity>& link = links[i];
		*link.ref = finder.find(link.target_name);
	}

	for (s32 i = 0; i < transform_links.length; i++)
	{
		LevelLink<Transform>& link = transform_links[i];
		*link.ref = finder.find(link.target_name)->get<Transform>();
	}

	for (s32 i = 0; i < finder.map.length; i++)
		World::awake(finder.map[i].entity.ref());

	Physics::sync_static();

	for (s32 i = 0; i < mover_links.length; i++)
	{
		MoverEntry& link = mover_links[i];
		Entity* object = finder.find(link.object_name);
		Entity* end = finder.find(link.end_name);
		link.mover->setup(object->get<Transform>(), end->get<Transform>(), link.speed);
	}

	for (s32 i = 0; i < ropes.length; i++)
		Rope::spawn(ropes[i].pos, ropes[i].rot * Vec3(0, 1, 0), ropes[i].max_distance, ropes[i].slack);

	// Set map view for local players
	{
		Entity* map_view = finder.find("map_view");
		if (map_view && map_view->has<Transform>())
		{
			for (auto i = LocalPlayer::list.iterator(); !i.is_last(); i.next())
				i.item()->map_view = map_view->get<Transform>();
		}
	}

	for (s32 i = 0; i < scripts.length; i++)
		scripts[i]->function(u, finder);

	Loader::level_free(json);

	for (s32 i = 0; i < Team::list.length; i++)
		Team::list[i].awake();
}

}
