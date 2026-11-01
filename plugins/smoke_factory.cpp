// Smoke Factory -- longer-lasting, bigger, rising tire smoke, as plugins\mt_plugin_SmokeFactory.dll.
//
// Tire smoke is the Cascade system /Game/Cars/Particles/P_TireSmoke. One patch on the
// shared template, no per-frame work; everything below is read live by the emitters.
//
//  - Lifetime and size are particle *parameters* ("LifeTime", "SizeScale") that the
//    distribution maps from its input range onto its output range at every spawn, so
//    scaling the output range (and the default Constant) by k scales them by k.
//  - Rise: the start velocity (ParticleModuleVelocity, baked uniform +-50 per axis)
//    gets RiseSpeed added to its Z, and the drag (ParticleModuleAccelerationDrag,
//    baked 0.5 - 0.9) is scaled by DragScale so the rise is not stopped within a
//    second. Baked tables are reflected on UE 5 (Table.Values): [min, max] for the
//    drag, [minX minY minZ maxX maxY maxZ] for the velocity.
//  - Less drag would also let a puff ride the car's velocity further, so the
//    velocity it inherits ("VehicleVelocity", passed through in Direct mode) is
//    mapped to InheritScale of it -- travel ~ speed / drag stays the same when both
//    scale alike, and the smoke still stays where the car slid.
//
// Settings: Binaries\Win64\smoke_factory.ini, [Smoke]. Missing keys are written with their
// defaults; change them and restart the game.

#include <cmath>
#include <set>
#include <string>
#include "../include/mt_plugin.hpp"

using namespace mt;

enum { DPM_Normal = 0, DPM_Abs = 1, DPM_Direct = 2 };

// FNames compare without case and keep the spelling they were first made with:
// the engine already has a property "Lifetime", so this parameter reads "Lifetime".
static bool same(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

static bool inTireSmoke(uintptr_t o) {
    for (int d = 0; d < 6 && o; d++, o = outerOf(o))
        if (objName(o) == "P_TireSmoke") return true;
    return false;
}

// Async loading creates an object before it is serialized: until these clear, its
// name and tables are still empty and anything written is overwritten by the load.
static bool loading(uintptr_t o) { return (get<uint32_t>(o + L().obj_flags) & 0x3400) != 0; }  // RF_NeedLoad|NeedPostLoad|NeedPostLoadSubobjects

static void scaleF(uintptr_t at, double k) { float v = get<float>(at); v = (float)(v * k); wr(at, &v, 4); }
static void scaleV(uintptr_t at, double k) {  // FVector is double precision on UE 5
    double v[3]; if (!rd(at, v, 24)) return;
    for (double& x : v) x *= k;
    wr(at, v, 24);
}
static void setV(uintptr_t at, double x) { double v[3] = {x, x, x}; wr(at, v, 24); }

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Smoke Factory loaded: tire smoke lasts longer, spreads wider and rises");
      struct { const wchar_t* key; double v; } cfg[] = {
          {L"LifetimeScale", 4.0}, {L"SizeScale", 1.5}, {L"RiseSpeed", 40.0}, {L"DragScale", 0.3}, {L"InheritScale", 0.3}};
      for (auto& c : cfg) {
          c.v = iniNum(L"smoke_factory.ini", L"Smoke", c.key, c.v);
          MT_LOG("setting %ls = %g", c.key, c.v);
      }
    double kLife = cfg[0].v, kSize = cfg[1].v, rise = cfg[2].v, kDrag = cfg[3].v, kInherit = cfg[4].v;
    if (kLife <= 0 || kSize <= 0 || kDrag <= 0 || kInherit < 0) {
        MT_LOG("FAILED: LifetimeScale, SizeScale and DragScale must be > 0, InheritScale >= 0 -- smoke stays vanilla");
        return 0;
    }

    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }
    uintptr_t cF = api->find_object("Class", "DistributionFloatParticleParameter", "/Script/Engine");
    uintptr_t cV = api->find_object("Class", "DistributionVectorParticleParameter", "/Script/Engine");
    uintptr_t cDrag = api->find_object("Class", "ParticleModuleAccelerationDrag", "/Script/Engine");
    uintptr_t cVel = api->find_object("Class", "ParticleModuleVelocity", "/Script/Engine");
    uintptr_t sRDF = api->find_object("ScriptStruct", "RawDistributionFloat", "/Script/Engine");
    uintptr_t sRDV = api->find_object("ScriptStruct", "RawDistributionVector", "/Script/Engine");
    uintptr_t sDLT = api->find_object("ScriptStruct", "DistributionLookupTable", "/Script/Engine");
    int fName = off(cF, "ParameterName"), fMinO = off(cF, "MinOutput"), fMaxO = off(cF, "MaxOutput"),
        fConst = off(cF, "Constant"), fMode = off(cF, "ParamMode");
    int vName = off(cV, "ParameterName"), vMinI = off(cV, "MinInput"), vMaxI = off(cV, "MaxInput"),
        vMinO = off(cV, "MinOutput"), vMaxO = off(cV, "MaxOutput"), vConst = off(cV, "Constant"), vModes = off(cV, "ParamModes");
    int dragProp = off(cDrag, "DragCoefficientRaw"), velProp = off(cVel, "StartVelocity");
    int rdfTable = off(sRDF, "Table"), rdvTable = off(sRDV, "Table"), rdvDist = off(sRDV, "Distribution");
    int dltValues = off(sDLT, "Values");
    MT_LOG("params: name+%x/%x in+%x/%x out+%x/%x | drag+%x vel+%x table+%x/%x dist+%x values+%x",
           fName, vName, vMinI, vMaxI, vMinO, vMaxO, dragProp, velProp, rdfTable, rdvTable, rdvDist, dltValues);
    if (!cF || !cV || fName < 0 || fMinO < 0 || fMaxO < 0 || fConst < 0 || vName < 0 || vMinO < 0 || vMaxO < 0 || vConst < 0) {
        MT_LOG("FAILED: particle parameter layout not found -- smoke stays vanilla");
        return 0;
    }
    bool canRise = cDrag && cVel && dragProp >= 0 && velProp >= 0 && rdfTable >= 0 && rdvTable >= 0 && rdvDist >= 0 &&
                   dltValues >= 0 && vMinI >= 0 && vMaxI >= 0 && vModes >= 0;
    if (!canRise) MT_LOG("WARNING: velocity/drag layout not found -- smoke lasts and grows, but does not rise");

    // P_TireSmoke loads with the first vehicle; keep looking (and re-patch if it is reloaded)
    std::set<uintptr_t> done;
    for (;;) {
        for (int i = 0, n = api->num_objects(); i < n; i++) {
            uintptr_t o = api->object(i);
            if (!o || isCDO(o) || done.count(o)) continue;
            uintptr_t k = classOf(o);
            if (k != cF && k != cV && !(canRise && (k == cDrag || k == cVel))) continue;
            if (!inTireSmoke(o) || loading(o)) continue;
            done.insert(o);
            if (k == cF) {
                std::string p = fname(o + fName);
                if (p.empty() || p == "None") { done.erase(o); continue; }  // not read in yet
                if (!same(p, "LifeTime")) continue;
                if (fMode >= 0 && get<uint8_t>(o + fMode) == DPM_Direct)
                    MT_LOG("WARNING: LifeTime is in Direct mode -- only the default (Constant) is scaled");
                scaleF(o + fMinO, kLife); scaleF(o + fMaxO, kLife); scaleF(o + fConst, kLife);
                MT_LOG("LifeTime: output now %.2f..%.2f, default %.2f (x%g)",
                       get<float>(o + fMinO), get<float>(o + fMaxO), get<float>(o + fConst), kLife);
            } else if (k == cV) {
                std::string p = fname(o + vName);
                if (p.empty() || p == "None") { done.erase(o); continue; }
                if (same(p, "SizeScale")) {
                    if (get<uint8_t>(o + vModes) == DPM_Direct)
                        MT_LOG("WARNING: SizeScale is in Direct mode -- only the default (Constant) is scaled");
                    scaleV(o + vMinO, kSize); scaleV(o + vMaxO, kSize); scaleV(o + vConst, kSize);
                    MT_LOG("SizeScale: output and default x%g", kSize);
                } else if (same(p, "VehicleVelocity") && canRise && kInherit != 1) {
                    // Direct passes the car's velocity straight through; Normal maps an input
                    // range onto an output range, so +-L -> +-L*k scales it by k (|v| < L).
                    const double L = 20000;  // cm/s, 720 km/h
                    setV(o + vMinI, -L); setV(o + vMaxI, L); setV(o + vMinO, -L * kInherit); setV(o + vMaxO, L * kInherit);
                    uint8_t modes[3] = {DPM_Normal, DPM_Normal, DPM_Normal};
                    wr(o + vModes, modes, 3);
                    MT_LOG("VehicleVelocity: puffs inherit x%g of the car's speed", kInherit);
                }
            } else if (k == cDrag && kDrag != 1) {
                uintptr_t values = o + dragProp + rdfTable + dltValues;
                uintptr_t d = get<uintptr_t>(values); int n2 = get<int>(values + 8);
                if (n2 <= 0) { done.erase(o); continue; }  // not baked yet
                for (int j = 0; j < n2 && j < 64; j++) scaleF(d + j * 4ull, kDrag);
                MT_LOG("drag: %d baked values x%g (now %.3f..%.3f)", n2, kDrag,
                       n2 > 0 ? get<float>(d) : 0.f, n2 > 1 ? get<float>(d + 4) : 0.f);
            } else if (k == cVel && rise != 0) {
                // the module with a baked uniform start velocity -- not the one fed by VehicleVelocity
                if (get<uintptr_t>(o + velProp + rdvDist)) continue;
                uintptr_t values = o + velProp + rdvTable + dltValues;
                uintptr_t d = get<uintptr_t>(values); int n2 = get<int>(values + 8);
                if (n2 <= 0) { done.erase(o); continue; }
                if (n2 != 6) { MT_LOG("start velocity: %d baked values, expected 6 -- not raised", n2); continue; }
                float z0 = get<float>(d + 8) + (float)rise, z1 = get<float>(d + 20) + (float)rise;
                wr(d + 8, &z0, 4); wr(d + 20, &z1, 4);
                MT_LOG("start velocity: Z now %.1f..%.1f (+%g up)", z1, z0, rise);
            }
        }
        Sleep(3000);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    // needs only the v1 api (reflection, read/write)
    if (!a || a->version < 1 || a->size < offsetof(MtApi, plugin) + sizeof(void*)) return 1;
    api = a;
    MoveFileW((gameDir() + L"smoke.ini").c_str(), (gameDir() + L"smoke_factory.ini").c_str());  // settings from before the rename
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
