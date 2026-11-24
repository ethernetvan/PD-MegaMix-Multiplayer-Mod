#pragma once
/*
 * PC port: game structures for DivaMegaMix.exe (Mega Mix+ 1.03, buildid
 * 11004484). Subset of New Classics' diva.h (MIT, github.com/korenkonder/
 * AFT_ModLoader's New Classics — see third_party/LICENSE.newclassics.txt),
 * keeping only what the multiplayer port reads, plus the mp-facing pvsel
 * structs (Switch-layout shadow elements).
 */
#include <cstdint>
#include <cstddef>
#include <string>
#include "../platform/NxShim.hpp"
#include "hooks.h"
#include "addresses.h"

namespace prj {
using string = std::string; /* game is MSVC-built; same layout, read-only use */
template <typename T>
struct vector { T* begin_; T* end_; T* cap_; };
} // namespace prj

struct PVGameData;

namespace diva {
struct vec2 { float x, y; };
struct vec3 { float x, y, z; };

struct SpriteVertex
{
	diva::vec3 pos;
	diva::vec2 uv;
	uint32_t color;
};

struct PvDscTarget
{
	int32_t type;
	diva::vec2 target_pos;
	diva::vec2 start_pos;
	float amplitude;
	int32_t frequency;
	bool slide_chain_start;
	bool slide_chain_end;
	bool slide_chain_left;
	bool slide_chain_right;
	bool slide_chain;
};

struct PvDscTargetGroup
{
	int32_t target_count;
	PvDscTarget targets[4];
	int32_t field_94;
	int64_t spawn_time;
	int64_t hit_time;
	bool slide_chain;
};

/* NOTE: "struc_14" in nc — type/difficulty of the running game mode. */
struct PvGameplayInfo
{
	int32_t type;
	int32_t difficulty;
};

struct PVGamePvData
{
	bool field_0;
	int32_t dsc_state;
	bool play;
	int script_buffer[45000];
	int script_pos;
	uint8_t gap2BF30[24];
	float float2BF48;
	uint8_t gap2BF4C[36];
	int int2BF70;
	int int2BF74;
	PVGameData* pv_game;
	uint8_t gap2BF80[24];
	float float2BF98;
	float float2BF9C;
	int32_t dword2BFA0;
	int32_t dword2BFA4;
	uint64_t script_time;
	uint8_t gap2BFA8[16];
	uint8_t byte2BFC0;
	uint8_t byte2BFC1;
	uint8_t gap2BFC8[8];
	int32_t dword2BFCC;
	int32_t dword2BFD0;
	uint8_t byte2BFD4;
	uint8_t byte2BFD5;
	uint8_t byte2BFD6;
	uint8_t gap2BFD7[961];
	uint8_t byte2C398;
	int32_t dword2C39C;
	uint8_t gap2C3A0[8];
	uint8_t byte2C3A8;
	int32_t dword2C3AC;
	uint8_t gap2C3B0[272];
	uint8_t byte2C4C0;
	int32_t dword2C4C4;
	float float2C4C8;
	float float2C4CC;
	float float2C4D0;
	float float2C4D4;
	float float2C4D8;
	float float2C4DC;
	bool has_dsc_signature;
	bool is_dsc_ac200_format;
	uint8_t pad2C4E2[2];
	int targets_remaining;
	prj::vector<PvDscTargetGroup> targets;
	size_t target_index;
	float float2C508;
	char char2C50C;
	uint8_t gap2C50D[63];
	uint8_t gap2C54C[8];
	int32_t dsc_branch_mode;
	int32_t first_challenge_target_index;
	int32_t last_challenge_target_index;
	uint8_t gap2C558[704];
	uint8_t byte2C820;
	int32_t dword2C824;
	int32_t dword2C828;
	float float2C82C;
};

struct PVGameUI
{
	int32_t int00;
	int32_t aet_list[95];
	bool visibility[95];
	float frame_bottom_offset[2];
	float frame_bottom_dt[2];
	float frame_top_offset[2];
	float frame_top_dt[2];
	float top_offset[2];
	float bottom_offset[2];
	bool frame_visibility[2];
	int32_t frame_action[2];
	uint8_t gap380[368];
	int32_t life;
	bool draw_combo_counter;
	int32_t chance_txt_state;
	bool show_chance_txt;
	int32_t int39C;
	uint8_t gap39C2[8];
	int32_t combo_counter_state;
	float combo_counter_time;
	uint8_t gap3B0[12];
	int32_t combo_num;
	int32_t int3C0;
	diva::vec2 combo_counter_pos;
};

/* PC PVGameData — layout from nc diva.h. NOTE: PC has no clear_border /
 * clear_border_eff fields (MegaMix+ is FT-based); the mp telemetry emits 0. */
struct PVGameData
{
	bool loaded;
	bool paused;
	uint8_t byte2;
	uint8_t byte3;
	uint8_t gap4[196];
	PVGamePvData pv_data;
	uint8_t gap2C8F8[8];
	PVGameUI ui;
	uint8_t gap2CCCC[1520 - sizeof(PVGameUI)];
	uint8_t byte2CEF0;
	uint8_t gap2CEF1[39];
	const char char2CF18;
	uint8_t gap2CF19[23];
	int64_t qword2CF30;
	const char char2CF38;
	uint8_t gap2CF39[23];
	int64_t qword2CF50;
	const char char2CF58;
	uint8_t gap2CF59[23];
	int64_t qword2CF70;
	const char char2CF78;
	uint8_t gap2CF79[23];
	int64_t qword2CF90;
	const char char2CF98;
	uint8_t gap2CF99[23];
	int64_t qword2CFB0;
	const char char2CFB8;
	uint8_t gap2CFB9[23];
	int64_t qword2CFD0;
	const char char2CFD8;
	uint8_t gap2CFD9[23];
	int64_t qword2CFF0;
	const char char2CFF8;
	uint8_t gap2CFF9[23];
	int64_t qword2D010;
	uint8_t gap2D018[357];
	uint8_t byte2D17D;
	uint8_t gap2D17E[174];
	uint8_t gap2D22C[8];
	int32_t life;
	int32_t score;
	int32_t dword2D23C;
	bool scoring_enabled;
	uint8_t gap2D241[3];
	int32_t total_challenge_bonus;
	int32_t combo;
	int32_t total_surv_challenge_bonus;
	uint8_t gap2D250[8];
	int32_t challenge_combo;
	int32_t max_combo;
	int32_t judge_count[5];         /* includes WRONG */
	int32_t judge_count_correct[5]; /* excludes WRONG */
	uint8_t gap2D288[16];
	float float2D298;
	uint8_t gap2D29C[12];
	int int2D2A8;
	uint8_t gap2D2AC;
	uint8_t byte2D2AD;
	float float2D2B0;
	int16_t word2D2B4;
	uint8_t gap2D2B6[6];
	int32_t challenge_bonus;
	int32_t dword2D2C0;
	uint8_t gap2D2C4[8];
	int32_t reference_score;
	int32_t reference_score_with_life;
	int32_t dword2D2D4;
	prj::vector<int32_t> target_reference_scores;
	uint8_t gap2D2F0[8];
	int64_t qword2D2F8;
	uint8_t gap2D300[4];
	float percentage;
	float clear_border;     /* song+difficulty clear % (Switch +0x2D0D0) */
	float clear_border_eff; /* running border, reaches clear_border at the end */
	uint8_t gap2D310[13];
	uint8_t no_fail;        /* +0x2D31D: No Fail Mode of this song (from TaskPvGame's data) */
	uint8_t gap2D31E;
	uint8_t byte2D31F;
	int64_t challenge_time_start;
	int64_t challenge_time_end;
	int64_t pv_end_time;
	float pv_end_time_sec;
	float float2D33C;
	uint8_t gap2D340[4];
	float float2D344;
	int64_t qword2D348;
	float float2D350;
	float float2D354;
	int32_t dword2D358;
	int32_t dword2D35C;
	uint8_t gap2D360[5];
	bool has_success_note;
	uint8_t gap2D366[10];
	int32_t dword2D370;
	uint8_t byte2D374;
	uint8_t byte2D375;
	uint8_t gap2D376[30];
	int32_t current_frame;
	int32_t dword2D398;
	int32_t total_life_bonus;
	int32_t dword2D3A0;
	uint8_t gap2D3A4[4];
	uint8_t byte2D3A8;
	uint8_t byte2D3A9;
	uint8_t gap2D3AA;
	uint8_t byte2D3AB;
	uint8_t gap2D3AC[2];
	uint8_t byte2D3AE;
	bool is_success_branch;
	uint8_t gap2D3B0[16];
	int32_t dword2D3C0;
	uint8_t gap2D3C4[16];
	uint8_t byte2D3D4;
	uint8_t gap2D3D5[19];
	int32_t dword2D3E8;
	uint8_t gap2D3EC[2020];
	int64_t qword2DBD0;
	int64_t qword2DBD8;
	float float2DBE0;
	int32_t dword2DBE4;
	uint8_t byte2DBE8;
	uint8_t byte2DBE9;
	uint8_t byte2DBEA;
	float float2DBEC;
	uint8_t byte2DBF0;
	uint8_t gap2DBF1[11];
	float float2DBFC;
	uint8_t gap2DC00[684];
	int64_t int642DEAC;
};

struct PvGameTarget
{
	PvGameTarget* prev;
	PvGameTarget* next;
	int32_t dword10;
	int32_t target_type;
	float flying_time_remaining;
	float player_hit_time;
	float flying_time;
	float amplitude;
	float freq;
	float cur_freq;
	uint8_t gap30[4];
	int32_t dword34;
	uint8_t gap38[4];
	float dword3C;
	uint8_t gap40;
	bool slide_chain_start;
	bool slide_chain_end;
	bool slide_left;
	bool slide_right;
	bool slide_chain;
	uint8_t gap46[2];
	int64_t hit_time;
	uint8_t gap50[32];
	int32_t target_aet;
	int32_t button_aet;
	int32_t dword78;
	int32_t target_eff_aet;
	diva::vec2 target_pos;
	diva::vec2 button_pos;
	diva::vec2 delta_pos;
	diva::vec2 delta_pos_sq;
	diva::vec2 vecA0;
	SpriteVertex kiseki[40];
	bool note_active;
	uint8_t gap469[3];
	float out_start_time;
	uint8_t gap470[4];
	int32_t hit_state;
	int32_t multi_count;
	int32_t dword47C;
	float float480;
	uint8_t gap484[12];
	uint8_t byte490;
	bool b491;
	bool b492;
	bool b493;
	float button_opacity;
	float target_opacity;
	float sudden_appear_frame;
	float scaling_end_time;
	uint8_t gap4A4[4];
	int32_t sprite_index;
	uint8_t gap4AC[4];
	int64_t appear_time;
	int16_t word4B8;
	uint8_t gap4BA[2];
	float kiseki_width;
	int32_t target_index;
	int32_t dword4C4;
};

struct PVGameArcade
{
	bool play;
	uint8_t gap1[7];
	void* ptr08;
	uint8_t gap[16];
	PvGameTarget* target;
	PvGameTarget target_buffer[64];
	uint8_t gap13228[64];
	float target_time;
	float cool_early_window;
	float cool_late_window;
	float fine_early_window;
	float fine_late_window;
	float safe_early_window;
	float safe_late_window;
	float sad_early_window;
	float sad_late_window;
	uint8_t byte1328C;
	uint8_t byte1328D;
	bool bool1328E;
	uint8_t byte1328F;
	float current_time;
	int32_t int13294;
	int32_t hit_effects[64];
	int32_t hit_effect_index;
	uint8_t gap1339C[96];
	int32_t dword133FC;
	int32_t dword13400[3];
	int32_t slide_hit_effects[64];
	int32_t slide_hit_effect_index;
	uint8_t gap13510[8];
	float fl13518;
	float fl1351C;
	float fl13520;
	float fl13524;
	float fl13528;
	float fl1352C;
	float fl13530;
	bool mute_slide_chime;
	uint8_t gap13535[7];
	bool bool1353C[4];
	uint8_t gap13540[14];
	int8_t char1354E[4];
	float fl13554[2];
};

struct InputState
{
	uint8_t _data[6720];

	/* key bitsets (0xA2 keys, 3x64-bit words each):
	 *   tapped @0x00, down @0x30, repeat-tap @0x90, abs-tapped @0x19C8
	 * positions: int32 @0xE0 + index*4 (0x14-0x17 = stick axes)
	 * Verified via xdis on MM+ 1.03 (IsButtonTapped/Down/Repeat thunk
	 * targets 0x1402AAF80/0x1402AAFE0/0x1402AB070, abs @0x1402AB160). */
	float GetPosition(int32_t index);
	int32_t GetDevice();
	bool IsButtonDown(int32_t key);
	bool IsButtonTapped(int32_t key);
	bool IsButtonTappedAbs(int32_t key);
	bool IsButtonTappedOrRepeat(int32_t key);
	inline bool IsInputBlocked() { return *reinterpret_cast<bool*>(&_data[0x1A28]); }
	inline uint64_t* KeysTapped()    { return reinterpret_cast<uint64_t*>(&_data[addr::InputOffTapped]); }
	inline uint64_t* KeysDown()      { return reinterpret_cast<uint64_t*>(&_data[addr::InputOffDown]); }
	inline uint64_t* KeysRepeat()    { return reinterpret_cast<uint64_t*>(&_data[addr::InputOffRepeat]); }
	inline uint64_t* KeysAbsTapped() { return reinterpret_cast<uint64_t*>(&_data[addr::InputOffAbsTapped]); }
	inline uint64_t* KeysAt(size_t off) { return reinterpret_cast<uint64_t*>(&_data[off]); }
	inline int32_t* Positions()      { return reinterpret_cast<int32_t*>(&_data[addr::InputOffPositions]); }
};

struct GlobalPVInfo
{
	int32_t difficulty; /* extra-extreme = 4 here, not 3+edition 1 */
	int32_t pv_id;
};

struct PVLoadParam
{
	struct Data
	{
		int32_t difficulty;
		int32_t edition;
		int32_t int8;
		uint8_t gap0[0x50];
		int32_t pv_id;
		uint8_t gap60[0x648];
	};
	Data data[4];
};

namespace pv_db {
	struct PvDBDifficulty
	{
		int32_t difficulty;
		int32_t edition;
		int32_t extra;
		int32_t dwordC;
		int64_t attributes;
		int32_t gap18[2];
		prj::string script_file_name;
		int32_t level;
		int32_t level_sort_index;
		prj::string button_se;
		prj::string success_se;
		prj::string slide_se;
		prj::string slidechain_start_se;
		prj::string slidechain_se;
		prj::string slidechain_success_se;
		prj::string slidechain_failure_se;
		prj::string slide_touch_se;
		uint8_t gap148[932];
		int32_t script_format;
		uint8_t gap4F0[64];
	};

	struct PvDBEntry
	{
		int32_t pv_id;
		int32_t date;
		prj::string song_name;
		prj::string song_name_reading;
		prj::string song_file_name;
		prj::string arrange_name;
		uint8_t gap80[0x68];
		prj::vector<PvDBDifficulty*> difficulty;
		prj::vector<int32_t> attribute;
	};

	inline FUNCTION_PTR(PvDBEntry*, __fastcall, FindPVEntry, addr::pv_dbFindPVEntry, int32_t id);
}

} // namespace diva

enum HitState : int32_t
{
	HitState_Cool       = 0,
	HitState_Fine       = 1,
	HitState_Safe       = 2,
	HitState_Sad        = 3,
	HitState_WrongCool  = 4,
	HitState_WrongFine  = 5,
	HitState_WrongSafe  = 6,
	HitState_WrongSad   = 7,
	HitState_Worst      = 8,
	HitState_CoolDouble = 9,
	HitState_FineDouble = 10,
	HitState_SafeDouble = 11,
	HitState_SadDouble  = 12,
	HitState_CoolTriple = 13,
	HitState_FineTriple = 14,
	HitState_SafeTriple = 15,
	HitState_SadTriple  = 16,
	HitState_CoolQuad   = 17,
	HitState_FineQuad   = 18,
	HitState_SafeQuad   = 19,
	HitState_SadQuad    = 20,
	HitState_None       = 21
};

/* pvsel element types — used by the Switch-layout shadow and the lobby's
 * sorted-list reads. On PC the MegaMix (Switch) selector stores PvData
 * elements and the FT selector stores PvDataPS4 elements. */
namespace pvsel {
struct PvData2 {
	int32_t* pv_db_entry;
	uint8_t gap[120];
	int32_t name_sort_index;
};
struct PvData {
	PvData2* data2;
	uint8_t data[0x40];
};
struct PvDataPS4 {
	PvData2* data2;
	uint8_t data[0x48]; /* 0x50 total element stride */
};
} // namespace pvsel

/* game-facing globals the lobby uses */
using diva::PVGameArcade;
using diva::PvGameTarget;
extern PVGameArcade* g_current_game; /* nullptr on PC for H1 */

namespace diva { namespace game {
	inline FUNCTION_PTR(InputState*, __fastcall, GetInputState, addr::GetInputState, int32_t index);
	inline FUNCTION_PTR(bool, __fastcall, CheckButtonDelegate, 0x1402AB1A0,
		InputState* state, int32_t key, bool(__fastcall* func)(InputState*, int32_t));
	inline FUNCTION_PTR(PvGameplayInfo*, __fastcall, GetPvGameplayInfo, 0x14027DD90);
	inline bool IsFutureToneMode() { return *reinterpret_cast<bool*>(addr::IsFutureToneMode); }
	inline FUNCTION_PTR(PVGameData*, __fastcall, GetPVGameData, addr::GetPVGameData);
	inline FUNCTION_PTR(PVLoadParam*, __fastcall, GetPVLoadParam, addr::GetPVLoadParam);
	inline FUNCTION_PTR(GlobalPVInfo*, __fastcall, GetGlobalPVInfo, addr::GetGlobalPVInfo);
	inline FUNCTION_PTR(bool, __fastcall, IsInSongResults, addr::IsInSongResults);
} } // namespace diva::game

