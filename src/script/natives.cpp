// Native methods for the script VM. Semantics follow the exe implementations
// documented in notes/06-native-methods.md and the shipped 2.2.1 java sources.
// Engine-dependent (or not yet reconstructed) methods are registered as explicit
// stubs that fail with their qualified name instead of silently doing nothing.
#include "script/natives.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <string>
#include <vector>

#include "engine/math.h"
#include "game/runtime.h"
#include "script/vm.h"

namespace {

using Args = std::vector<Value>;

bool want(const char *who, const Args &a, size_t n, std::string *err)
{
    if (a.size() < n) {
        *err = std::string(who) + ": expected " + std::to_string(n) + " argument(s)";
        return false;
    }
    return true;
}

bool get_i(const char *who, const Value &v, int32_t *out, std::string *err)
{
    if (v.kind != Value::Int) {
        *err = std::string(who) + ": expected an int";
        return false;
    }
    *out = v.i;
    return true;
}

bool get_f(const char *who, const Value &v, float *out, std::string *err)
{
    if (v.kind == Value::Float) {
        *out = v.f;
        return true;
    }
    if (v.kind == Value::Int) {  // callMethod converts int args for F parameters
        *out = (float)v.i;
        return true;
    }
    *err = std::string(who) + ": expected a float";
    return false;
}

bool get_s(const char *who, const Value &v, const std::string **out, std::string *err)
{
    if (v.kind != Value::Str) {
        *err = std::string(who) + ": expected a String";
        return false;
    }
    *out = &v.s;
    return true;
}

bool get_o(const char *who, const Value &v, Object **out, std::string *err)
{
    if (v.kind != Value::Obj || !v.obj) {
        *err = std::string(who) + ": expected an object";
        return false;
    }
    *out = v.obj;
    return true;
}

// ---- java.lang.Math --------------------------------------------------------

// MSVC CRT rand/srand state (0x571110/0x571131): state = state*214013+2531011,
// result = (state >> 16) & 0x7fff. The CRT starts seeded with 1.
uint32_t g_rand_state = 1;

uint32_t msvc_rand()
{
    g_rand_state = g_rand_state * 214013u + 2531011u;
    return (g_rand_state >> 16) & 0x7fffu;
}

void msvc_srand(uint32_t seed)
{
    g_rand_state = seed;
}

// 0x569730: high-resolution clock in milliseconds. The exe subtracts a
// process-lifetime baseline; ours starts at the first call, which is equivalent
// for the monotonic differences scripts use.
double milliseconds_now()
{
    static const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

bool math_random(Vm &, const Args &, Value *out, std::string *)
{
    // 0x4dee50: 0x7fff maps to 0x7ffe so the result stays below 1.
    uint32_t u = msvc_rand();
    if (u == 0x7fffu) u = 0x7ffeu;
    *out = Value::make_float((float)(int)u * (float)(1.0 / 32767.0));
    return true;
}

bool math_sqrt(Vm &, const Args &a, Value *out, std::string *err)
{
    float x;
    if (!want("Math.sqrt", a, 1, err) || !get_f("Math.sqrt", a[0], &x, err))
        return false;
    *out = Value::make_float(sqrtf(x));  // 0x4deee0: x87 fsqrt, no domain check
    return true;
}

bool math_randomize(Vm &, const Args &, Value *out, std::string *)
{
    // 0x4dee90: srand((int)milliseconds since process start).
    msvc_srand((uint32_t)(int)milliseconds_now());
    *out = Value::make_null();
    return true;
}

bool math_setrandseed(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t seed;
    if (!want("Math.setrandseed", a, 1, err) ||
        !get_i("Math.setrandseed", a[0], &seed, err))
        return false;
    msvc_srand((uint32_t)seed);
    *out = Value::make_null();
    return true;
}

bool math_ln(Vm &, const Args &a, Value *out, std::string *err)
{
    float x;
    if (!want("Math.ln", a, 1, err) || !get_f("Math.ln", a[0], &x, err))
        return false;
    // 0x4decf0: logs bad input but still returns logf. log10 is registered at
    // the same address, so it returns the natural log too (game bug).
    if (x <= 0.0f) fprintf(stderr, "vm_math_ln: incorrect argument %f\n", (double)x);
    *out = Value::make_float(logf(x));
    return true;
}

bool math_exp(Vm &, const Args &a, Value *out, std::string *err)
{
    float x;
    if (!want("Math.exp", a, 1, err) || !get_f("Math.exp", a[0], &x, err))
        return false;
    *out = Value::make_float(expf(x));  // 0x4decc0
    return true;
}

bool math_pow(Vm &, const Args &a, Value *out, std::string *err)
{
    float x, y;
    if (!want("Math.pow", a, 2, err) || !get_f("Math.pow", a[0], &x, err) ||
        !get_f("Math.pow", a[1], &y, err))
        return false;
    *out = Value::make_float(powf(x, y));  // 0x4dee10 -> SSE powf 0x571143
    return true;
}

bool math_atan2(Vm &, const Args &a, Value *out, std::string *err)
{
    float y, x;
    if (!want("Math.atan2", a, 2, err) || !get_f("Math.atan2", a[0], &y, err) ||
        !get_f("Math.atan2", a[1], &x, err))
        return false;
    *out = Value::make_float(atan2f(y, x));  // 0x4dec90: fpatan(arg0, arg1)
    return true;
}

// ---- java.lang.System ------------------------------------------------------

bool noop_void(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

bool zero_int(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(0);  // 0x461910 runFinalization returns 0
    return true;
}

bool system_arraycopy(Vm &, const Args &a, Value *out, std::string *err)
{
    const char *who = "System.arraycopy";
    if (!want(who, a, 5, err)) return false;
    if (a[0].kind != Value::Array || a[2].kind != Value::Array) {
        *err = std::string(who) + ": expected arrays";
        return false;
    }
    int32_t src_pos, dst_pos, len;
    if (!get_i(who, a[1], &src_pos, err) || !get_i(who, a[3], &dst_pos, err) ||
        !get_i(who, a[4], &len, err))
        return false;
    Array *src = a[0].arr;
    Array *dst = a[2].arr;
    // 0x4e5280 checks each upper bound and returns without copying. The exe
    // would index out of range for negative values; treat them as failed bounds.
    if (src_pos < 0 || dst_pos < 0 || len < 0 ||
        (int64_t)src->elems.size() < (int64_t)src_pos + len ||
        (int64_t)dst->elems.size() < (int64_t)dst_pos + len) {
        *out = Value::make_null();
        return true;
    }
    if (src == dst) {
        std::vector<Value> tmp(src->elems.begin() + src_pos,
                               src->elems.begin() + src_pos + len);
        for (int32_t i = 0; i < len; i++) dst->elems[dst_pos + i] = tmp[(size_t)i];
    } else {
        for (int32_t i = 0; i < len; i++)
            dst->elems[dst_pos + i] = src->elems[src_pos + i];
    }
    *out = Value::make_null();
    return true;
}

bool system_current_time(Vm &, const Args &, Value *out, std::string *)
{
    // 0x4e5500: milliseconds * 0.001, returned as float seconds.
    *out = Value::make_float((float)(milliseconds_now() * 0.001));
    return true;
}

bool system_build_number(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(940);  // 0x4e5470 returns 0x3ac
    return true;
}

// ---- java.lang.Integer / Float --------------------------------------------

bool int_tostring(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t v;
    if (!want("Integer.toString", a, 1, err) ||
        !get_i("Integer.toString", a[0], &v, err))
        return false;
    char buf[1024];
    snprintf(buf, sizeof buf, "%d", (int)v);  // 0x4deb80
    *out = Value::make_str(buf);
    return true;
}

bool int_tohexstring(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t v;
    if (!want("Integer.toHexString", a, 1, err) ||
        !get_i("Integer.toHexString", a[0], &v, err))
        return false;
    char buf[1024];
    snprintf(buf, sizeof buf, "0x%08x", (unsigned)v);  // 0x4deb30
    *out = Value::make_str(buf);
    return true;
}

bool float_tostring(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    const std::string *fmt;
    if (!want("Float.toString", a, 2, err) || !get_f("Float.toString", a[0], &v, err) ||
        !get_s("Float.toString", a[1], &fmt, err))
        return false;
    char buf[1024];
    // 0x4da0e0 passes the script format string straight to sprintf. Format
    // errors are the script's problem, exactly like the original.
    snprintf(buf, sizeof buf, fmt->c_str(), (double)v);
    *out = Value::make_str(buf);
    return true;
}

// ---- java.lang.String ------------------------------------------------------

// 0x4e4f70 drops the native string handle's reference. Rebuild strings are
// owned values without a handle or refcount, so there is nothing to release.
bool string_finalize(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

bool string_length(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.length", a, 1, err) ||
        !get_s("String.length", a[0], &s, err))
        return false;
    *out = Value::make_int((int32_t)s->size());  // 0x4e5110: strlen
    return true;
}

bool string_intvalue(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.intValue", a, 1, err) ||
        !get_s("String.intValue", a[0], &s, err))
        return false;
    int v = 0;
    // shortcut: the exe leaves the stack slot uninitialized on parse failure;
    // zero is deterministic here.
    sscanf(s->c_str(), "%d", &v);  // 0x4e50d0 via sscanf
    *out = Value::make_int(v);
    return true;
}

bool string_floatvalue(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.floatValue", a, 1, err) ||
        !get_s("String.floatValue", a[0], &s, err))
        return false;
    float v = 0.0f;
    sscanf(s->c_str(), "%f", &v);  // 0x4e4fc0 via sscanf
    *out = Value::make_float(v);
    return true;
}

bool string_append(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    int32_t ascii;
    if (!want("String.append", a, 2, err) ||
        !get_s("String.append", a[0], &s, err) ||
        !get_i("String.append", a[1], &ascii, err))
        return false;
    std::string r = *s;
    if (r.size() > 255) r.resize(255);  // 0x4e4ed0 copies into char[256]
    if (ascii == 8) {
        if (!r.empty()) r.pop_back();
    } else if (ascii > 0x1f && r.size() < 255) {
        r.push_back((char)ascii);
    }
    *out = Value::make_str(r);
    return true;
}

bool string_token(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s, *delims;
    int32_t n;
    if (!want("String.token", a, 3, err) || !get_s("String.token", a[0], &s, err) ||
        !get_i("String.token", a[1], &n, err) || !get_s("String.token", a[2], &delims, err))
        return false;
    std::string copy = *s;
    char *save = nullptr;
    char *tok = nullptr;
    char *p = copy.data();
    if (n < 0) {
        // Negative counts exhaust the string in the exe's loop and return null.
        while (strtok_r(p, delims->c_str(), &save)) p = nullptr;
    } else {
        for (int32_t i = 0; i <= n; i++) {
            tok = strtok_r(p, delims->c_str(), &save);
            p = nullptr;
            if (!tok) break;
        }
    }
    *out = tok ? Value::make_str(tok) : Value::make_null();
    return true;
}

bool string_getascii(Vm &vm, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.getASCII", a, 1, err) ||
        !get_s("String.getASCII", a[0], &s, err))
        return false;
    Array *arr = vm.new_array("I", s->size());
    for (size_t i = 0; i < s->size(); i++)
        arr->elems[i] = Value::make_int((int32_t)(signed char)(*s)[i]);  // char is signed
    *out = Value::make_array(arr);
    return true;
}

// ---- Vector3 / Ypr ---------------------------------------------------------

bool read_f(Vm &vm, Object *o, const char *name, float *out, std::string *err)
{
    Value v;
    if (!vm.get_instance_field(o, name, &v, err)) return false;
    return get_f(name, v, out, err);
}

bool write_f(Vm &vm, Object *o, const char *name, float value, std::string *err)
{
    return vm.set_instance_field(o, name, Value::make_float(value), err);
}

bool vec_read(Vm &vm, Object *o, float v[3], std::string *err)
{
    return read_f(vm, o, "x", &v[0], err) && read_f(vm, o, "y", &v[1], err) &&
           read_f(vm, o, "z", &v[2], err);
}

bool vec_write(Vm &vm, Object *o, const float v[3], std::string *err)
{
    return write_f(vm, o, "x", v[0], err) && write_f(vm, o, "y", v[1], err) &&
           write_f(vm, o, "z", v[2], err);
}

bool ypr_read(Vm &vm, Object *o, float v[3], std::string *err)
{
    return read_f(vm, o, "y", &v[0], err) && read_f(vm, o, "p", &v[1], err) &&
           read_f(vm, o, "r", &v[2], err);
}

bool ypr_write(Vm &vm, Object *o, const float v[3], std::string *err)
{
    return write_f(vm, o, "y", v[0], err) && write_f(vm, o, "p", v[1], err) &&
           write_f(vm, o, "r", v[2], err);
}

// 0x569970 Math__matToYpr.
void mat_to_ypr(const Mat34 &m, float out[3])
{
    float yaw, pitch, roll;
    if (m.m[10] == 0.0f && m.m[2] == 0.0f) {
        yaw = atan2f(m.m[0], -m.m[1]);
        pitch = 3.1415927f;
        roll = 0.0f;
    } else {
        yaw = atan2f(m.m[2], m.m[10]);
        roll = atan2f(m.m[4], m.m[5]);
        float c = cosf(roll);
        if (c > 0.0f)
            pitch = atan2f(-(m.m[6] * c), m.m[5]);
        else
            pitch = atan2f(m.m[6] * c, -m.m[5]);
    }
    out[0] = yaw;
    out[1] = pitch;
    out[2] = roll;
}

bool vec_length(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    float v[3];
    if (!want("Vector3.length", a, 1, err) ||
        !get_o("Vector3.length", a[0], &o, err) || !vec_read(vm, o, v, err))
        return false;
    *out = Value::make_float(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    return true;
}

bool vec_distance(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *p;
    float v[3], w[3];
    if (!want("Vector3.distance", a, 2, err) ||
        !get_o("Vector3.distance", a[0], &o, err) ||
        !get_o("Vector3.distance", a[1], &p, err) || !vec_read(vm, o, v, err) ||
        !vec_read(vm, p, w, err))
        return false;
    float dx = w[0] - v[0], dy = w[1] - v[1], dz = w[2] - v[2];
    *out = Value::make_float(sqrtf(dx * dx + dy * dy + dz * dz));
    return true;
}

bool vec_normalize(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    float v[3];
    if (!want("Vector3.normalize", a, 1, err) ||
        !get_o("Vector3.normalize", a[0], &o, err) || !vec_read(vm, o, v, err))
        return false;
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n != 0.0f) {
        float k = 1.0f / n;
        v[0] *= k;
        v[1] *= k;
        v[2] *= k;
        if (!vec_write(vm, o, v, err)) return false;
    }
    *out = Value::make_null();
    return true;
}

bool vec_rotate_ypr(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *yo;
    float v[3], ang[3];
    if (!want("Vector3.rotate", a, 2, err) ||
        !get_o("Vector3.rotate", a[0], &o, err) ||
        !get_o("Vector3.rotate", a[1], &yo, err) || !vec_read(vm, o, v, err) ||
        !ypr_read(vm, yo, ang, err))
        return false;
    Mat34 m;
    euler_to_mat(&m, ang);
    float nx = m.m[0] * v[0] + m.m[1] * v[1] + m.m[2] * v[2];
    float ny = m.m[4] * v[0] + m.m[5] * v[1] + m.m[6] * v[2];
    float nz = m.m[8] * v[0] + m.m[9] * v[1] + m.m[10] * v[2];
    float nv[3] = {nx, ny, nz};
    if (!vec_write(vm, o, nv, err)) return false;
    *out = a[0];  // 0x4e67e0 returns the receiver handle
    return true;
}

bool vec_rotate_axis(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *ao;
    float v[3], axis[3], angle;
    if (!want("Vector3.rotate", a, 3, err) ||
        !get_o("Vector3.rotate", a[0], &o, err) ||
        !get_o("Vector3.rotate", a[1], &ao, err) ||
        !get_f("Vector3.rotate", a[2], &angle, err) || !vec_read(vm, o, v, err) ||
        !vec_read(vm, ao, axis, err))
        return false;
    float ax = axis[0], ay = axis[1], az = axis[2];
    float norm2 = ax * ax + ay * ay + az * az;
    if (norm2 < 1e-6f) {
        // shortcut: 0x4e6650 multiplies by an uninitialized matrix for a zero
        // axis; leave the vector unchanged instead.
        *out = a[0];
        return true;
    }
    if (fabsf(norm2 - 1.0f) > 1e-6f) {
        float k = 1.0f / sqrtf(norm2);
        ax *= k;
        ay *= k;
        az *= k;
    }
    float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    Mat34 m;
    m.m[0] = t * ax * ax + c;
    m.m[1] = t * ax * ay - s * az;
    m.m[2] = t * ax * az + s * ay;
    m.m[4] = t * ax * ay + s * az;
    m.m[5] = t * ay * ay + c;
    m.m[6] = t * ay * az - s * ax;
    m.m[8] = t * ax * az - s * ay;
    m.m[9] = t * ay * az + s * ax;
    m.m[10] = t * az * az + c;
    float nx = m.m[0] * v[0] + m.m[1] * v[1] + m.m[2] * v[2];
    float ny = m.m[4] * v[0] + m.m[5] * v[1] + m.m[6] * v[2];
    float nz = m.m[8] * v[0] + m.m[9] * v[1] + m.m[10] * v[2];
    float nv[3] = {nx, ny, nz};
    if (!vec_write(vm, o, nv, err)) return false;
    *out = a[0];
    return true;
}

bool vec_ctor_ypr(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *yo;
    float ang[3];
    if (!want("Vector3.<init>", a, 2, err) ||
        !get_o("Vector3.<init>", a[0], &o, err) ||
        !get_o("Vector3.<init>", a[1], &yo, err) || !ypr_read(vm, yo, ang, err))
        return false;
    Mat34 m;
    euler_to_mat(&m, ang);
    float v[3] = {-m.m[2], -m.m[6], -m.m[10]};  // 0x4e6380
    if (!vec_write(vm, o, v, err)) return false;
    *out = Value::make_null();
    return true;
}

bool ypr_ctor_vector(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *vo;
    float v[3], ypr[3];
    if (!want("Ypr.<init>", a, 2, err) ||
        !get_o("Ypr.<init>", a[0], &o, err) ||
        !get_o("Ypr.<init>", a[1], &vo, err) || !vec_read(vm, vo, v, err))
        return false;
    if (v[0] == 0.0f && v[2] == 0.0f) {
        // 0x4e7170 -> 0x569a60: y == 0 takes the negative branch (pitch -pi/2).
        ypr[0] = 0.0f;
        ypr[1] = v[1] > 0.0f ? 1.5707964f : -1.5707964f;
        ypr[2] = 0.0f;
    } else {
        ypr[0] = atan2f(-v[0], -v[2]);
        ypr[1] = atan2f(v[1], sqrtf(v[0] * v[0] + v[2] * v[2]));
        ypr[2] = 0.0f;
    }
    if (!ypr_write(vm, o, ypr, err)) return false;
    *out = Value::make_null();
    return true;
}

bool ypr_mul(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *po;
    float left[3], right[3];
    if (!want("Ypr.mul", a, 2, err) || !get_o("Ypr.mul", a[0], &o, err) ||
        !get_o("Ypr.mul", a[1], &po, err) || !ypr_read(vm, o, left, err) ||
        !ypr_read(vm, po, right, err))
        return false;
    Mat34 A, B;
    euler_to_mat(&A, left);
    euler_to_mat(&B, right);
    // 0x566a70 product, in-place on A (column-major 3x3 layout).
    const float *b = B.m;
    float r[12];
    r[0] = A.m[0] * b[0] + A.m[4] * b[1] + A.m[8] * b[2];
    r[1] = A.m[1] * b[0] + A.m[5] * b[1] + A.m[9] * b[2];
    r[2] = A.m[2] * b[0] + A.m[6] * b[1] + A.m[10] * b[2];
    r[4] = b[4] * A.m[0] + b[5] * A.m[4] + b[6] * A.m[8];
    r[5] = b[4] * A.m[1] + b[5] * A.m[5] + b[6] * A.m[9];
    r[6] = b[4] * A.m[2] + b[5] * A.m[6] + b[6] * A.m[10];
    r[8] = b[8] * A.m[0] + b[9] * A.m[4] + b[10] * A.m[8];
    r[9] = b[8] * A.m[1] + b[9] * A.m[5] + b[10] * A.m[9];
    r[10] = b[8] * A.m[2] + b[9] * A.m[6] + b[10] * A.m[10];
    r[3] = r[7] = r[11] = 0.0f;
    Mat34 M;
    memcpy(M.m, r, sizeof M.m);
    float ypr[3];
    mat_to_ypr(M, ypr);
    if (!ypr_write(vm, o, ypr, err)) return false;
    *out = Value::make_null();
    return true;
}

// ---- java.lang.Object ------------------------------------------------------

// 0x4dfb60/0x4dfb80 relink the instance between GC lists in the exe VM.
// Rebuild objects are arena-owned and GC-less; no script-visible values change.
bool object_gc_toggle(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

bool object_hashcode(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Object.hashCode", a, 1, err) ||
        !get_o("Object.hashCode", a[0], &o, err))
        return false;
    *out = Value::make_int((int32_t)(intptr_t)o);  // 0x4dfc50 returns the handle
    return true;
}


// ---- java.util.resource.ResourceRef / GameRef / java.lang.GameType ---------

Runtime *game_rt()
{
    return Runtime::active();
}

bool need_rt(const char *who, std::string *err)
{
    if (game_rt()) return true;
    *err = std::string(who) + ": no active game runtime";
    return false;
}

Runtime::Handle *value_handle(const Value &v)
{
    Runtime *rt = game_rt();
    if (!rt || v.kind != Value::Obj || !v.obj) return nullptr;
    return rt->handle(v.obj);
}

Runtime::Resource *value_resource(const Value &v)
{
    Runtime::Handle *h = value_handle(v);
    return h ? h->target : nullptr;
}

bool receiver_object(const char *who, const Args &a, Object **out, std::string *err)
{
    if (!want(who, a, 1, err)) return false;
    return get_o(who, a[0], out, err);
}

std::string value_string(const Args &a, size_t i)
{
    return i < a.size() && a[i].kind == Value::Str ? a[i].s : std::string();
}

// Wrapper objects for getParent/getFirstChild and friends. The exe returns
// null when the field is null and logs when the handle itself is missing.
bool wrap_resource(Runtime::Resource *r, Value *out, std::string *err)
{
    *out = Value::make_null();
    if (!r) return true;
    Object *o = game_rt()->wrap(r, err);
    if (!o) {
        if (err->empty()) *err = "resource wrapper creation failed";
        return false;
    }
    *out = Value::make_obj(o);
    return true;
}

Object *new_point(Vm &vm, const char *cls, const float v[3], bool ypr, std::string *err)
{
    ClassState *cs = vm.find_class(cls);
    if (!cs) {
        *err = std::string(cls) + " is not loaded";
        return nullptr;
    }
    Object *o = vm.new_object(cs, err);
    if (!o) return nullptr;
    bool ok = ypr ? ypr_write(vm, o, v, err) : vec_write(vm, o, v, err);
    return ok ? o : nullptr;
}

bool read_vec_arg(Vm &vm, const char *who, const Value &v, float out[3], std::string *err)
{
    out[0] = out[1] = out[2] = 0.0f;
    if (v.kind == Value::Null) return true;  // setMatrix/setState zero defaults
    Object *o;
    if (!get_o(who, v, &o, err)) return false;
    return vec_read(vm, o, out, err);
}

bool read_ypr_arg(Vm &vm, const char *who, const Value &v, float out[3], std::string *err)
{
    out[0] = out[1] = out[2] = 0.0f;
    if (v.kind == Value::Null) return true;
    Object *o;
    if (!get_o(who, v, &o, err)) return false;
    return ypr_read(vm, o, out, err);
}

// 0x4e4080: allocate the 0x10-byte ref node and attach it to the instance.
bool rr_new_native(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.newNative", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.newNative", a, &o, err)) return false;
    game_rt()->attach(o);
    *out = Value::make_null();
    return true;
}

// 0x4e3220: unlink from the RI handle list, free the node, detach.
bool rr_delete_native(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.deleteNative", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.deleteNative", a, &o, err)) return false;
    game_rt()->detach(o);
    *out = Value::make_null();
    return true;
}

// 0x4e43a0 -> FUN_0051f9b0: unresolved ids clear the handle (the exe drops the
// old link, then only rebinds when FUN_005183a0 returns an instance).
bool rr_set_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.set", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.set", a, &o, err)) return false;
    int32_t id;
    if (!want("ResourceRef.set", a, 2, err) ||
        !get_i("ResourceRef.set", a[1], &id, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);
    if (h) game_rt()->bind(h, (uint32_t)id);
    *out = Value::make_null();
    return true;
}

// 0x4e4350 -> FUN_0040cac0: copy the argument RI (id = RI+0x80); a null or
// unbound argument clears the receiver.
bool rr_set_ref(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.set", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.set", a, &o, err)) return false;
    if (!want("ResourceRef.set", a, 2, err)) return false;
    Runtime::Handle *h = game_rt()->handle(o);
    if (h) {
        Runtime::Handle *src = value_handle(a[1]);
        if (src)
            game_rt()->bind(h, src->target);
        else
            game_rt()->bind(h, (Runtime::Resource *)nullptr);
    }
    *out = Value::make_null();
    return true;
}

// 0x4e43e0: RI ? RI+0x7c : 0. A missing handle logs and returns 0.
bool rr_type(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.type", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.type", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    *out = Value::make_int(r ? (int32_t)r->type : 0);
    return true;
}

// 0x4e3cd0: ref node +8.
bool rr_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.id", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.id", a, &o, err)) return false;
    Runtime::Handle *h = game_rt()->handle(o);
    *out = Value::make_int(h ? (int32_t)h->id : 0);
    return true;
}

// 0x4e36b0: parent RI @ +0x24, id @ +0x80; the record super chain here.
bool rr_get_parent_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getParentID", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getParentID", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    *out = Value::make_int(r ? (int32_t)r->parent_id : 0);
    return true;
}

// 0x4e37c0/FUN_004ea740: wrap the parent RI; null when there is none.
bool rr_get_parent(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getParent", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getParent", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    return wrap_resource(r ? r->parent : nullptr, out, err);
}

// 0x4e3c00/FUN_004ea900: RI+0xfc world-tree root; null when the field is null.
bool rr_get_wt_root(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getWTRoot", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getWTRoot", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    return wrap_resource(r ? r->world_root : nullptr, out, err);
}

// 0x4e3510/FUN_004e9b30: first child RI @ +0x30.
bool rr_get_first_child(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getFirstChild", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getFirstChild", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    Runtime::Resource *c = r && !r->children.empty() ? r->children[0] : nullptr;
    return wrap_resource(c, out, err);
}

// 0x4e35e0/FUN_004ea610: next sibling RI @ +0x4.
bool rr_get_next_child(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getNextChild", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getNextChild", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    return wrap_resource(r ? r->next_sibling : nullptr, out, err);
}

// ---- java.util.resource.GameRef -------------------------------------------

// 0x4daa40: RI+0x84. No missing-handle error in the exe; returns 0.
bool gr_get_flags(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getFlags", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getFlags", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    *out = Value::make_int(r ? (int32_t)r->flags : 0);
    return true;
}

// 0x4db900: RI+0x84 |= flags; bit 0x10 also pokes the world-tree update.
// shortcut: only the stored mask is modeled (no world-tree dispatcher).
bool gr_set_flags(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setFlags", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setFlags", a, &o, err)) return false;
    int32_t flags;
    if (!want("GameRef.setFlags", a, 2, err) ||
        !get_i("GameRef.setFlags", a[1], &flags, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) r->flags |= (uint32_t)flags;
    *out = Value::make_null();
    return true;
}

// 0x4da5e0: RI+0x84 &= ~flags.
bool gr_clear_flags(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.clearFlags", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.clearFlags", a, &o, err)) return false;
    int32_t flags;
    if (!want("GameRef.clearFlags", a, 2, err) ||
        !get_i("GameRef.clearFlags", a[1], &flags, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) r->flags &= ~(uint32_t)flags;
    *out = Value::make_null();
    return true;
}

// 0x4e3890: missing handle/id logs and returns null; otherwise a new Vector3.
bool gr_get_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getPos", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getPos", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Vector3", r->pos, false, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4dac40: FUN_004ea660 orientation, returned as Ypr.
bool gr_get_ori(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getOri", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getOri", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Ypr", r->ori, true, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4db050: FUN_0047c640(handle, 3, &v) (GII_VEL).
bool gr_get_vel(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getVel", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getVel", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Vector3", r->vel, false, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4e3a80/0x4dae20/0x4db240: fill the supplied object and return it; a missing
// handle logs and returns null (the argument is untouched).
bool gr_get_pos_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getPos", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.getPos", a, &o, err) ||
        !want("GameRef.getPos", a, 2, err) ||
        !get_o("GameRef.getPos", a[1], &v, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    if (!vec_write(vm, v, r->pos, err)) return false;
    *out = Value::make_obj(v);
    return true;
}

bool gr_get_ori_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getOri", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.getOri", a, &o, err) ||
        !want("GameRef.getOri", a, 2, err) ||
        !get_o("GameRef.getOri", a[1], &v, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    if (!ypr_write(vm, v, r->ori, err)) return false;
    *out = Value::make_obj(v);
    return true;
}

bool gr_get_vel_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getVel", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.getVel", a, &o, err) ||
        !want("GameRef.getVel", a, 2, err) ||
        !get_o("GameRef.getVel", a[1], &v, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    if (!vec_write(vm, v, r->vel, err)) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4e3330: detail float at RI+0x9c for instance/render resources, else 0.
bool gr_get_detail(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getDetail", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getDetail", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    float d = 0.0f;
    if (r && (r->type == 1 || r->type == 3)) d = r->detail;
    *out = Value::make_float(d);
    return true;
}

// 0x4db3b0: no RI or type != 8 -> 1. Type resources are empty when the record
// carries no data.
// shortcut: typed-data presence approximated by record/rsd data; upgrade when
// game-object typed data loading is modeled.
bool gr_is_empty(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.isEmpty", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.isEmpty", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    int32_t empty = 1;
    if (r && r->type == 8) {
        bool has_data = r->record &&
                        (r->record->data_size > 0 || (r->db && !r->db->rsd.empty()));
        empty = has_data ? 0 : 1;
    }
    *out = Value::make_int(empty);
    return true;
}

// 0x4db460: instance -> typed+0x50 script instance; type 8 -> type's script
// class, then the class chain check (FUN_00549d60). Null name skips the check.
bool gr_is_scripted(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.isScripted", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.isScripted", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    const std::string *name = nullptr;
    if (a.size() > 1 && a[1].kind == Value::Str) name = &a[1].s;
    std::string cls;
    int32_t scripted = 0;
    if (r && r->type == 1 && r->script) {
        scripted = 1;
        cls = r->script_class;  // the instance was made from the type's class
    } else if (r && r->type == 8 && !r->script_class.empty()) {
        scripted = 1;
        cls = r->script_class;
    }
    if (scripted && name) scripted = game_rt()->class_is_a(cls, *name) ? 1 : 0;
    *out = Value::make_int(scripted);
    return true;
}

// 0x4daf90: script instance of an instance, or one made for a type.
bool gr_get_script_instance(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getScriptInstance", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getScriptInstance", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    Object *s = game_rt()->script_instance(r);
    *out = s ? Value::make_obj(s) : Value::make_null();
    return true;
}

// 0x4da620: receiver handle required; type defaults to DAT_005fde50 in the exe
// (not modeled); a zero parent id short-circuits to null. createScripted binds
// the receiver handle to the new instance and returns its script instance.
bool gr_create(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.create", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.create", a, &o, err)) return false;
    if (!want("GameRef.create", a, 5, err)) return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);
    *out = Value::make_null();
    if (!h) return true;
    Runtime::Resource *parent = value_resource(a[1]);
    Runtime::Resource *type = value_resource(a[2]);
    if (!parent || parent->id == 0 || !type) return true;
    Runtime::Resource *inst = rt->instantiate(type, parent, value_string(a, 4),
                                              value_string(a, 3), true, err);
    if (!inst) return false;
    rt->bind(h, inst);
    if (inst->script) *out = Value::make_obj(inst->script);
    return true;
}

// 0x4da7e0: createNativeScripted, no script instance, receiver rebound.
bool gr_create_native(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.create_native", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.create_native", a, &o, err)) return false;
    if (!want("GameRef.create_native", a, 5, err)) return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);
    *out = Value::make_null();
    if (!h) return true;
    Runtime::Resource *parent = value_resource(a[1]);
    Runtime::Resource *type = value_resource(a[2]);
    if (!parent || parent->id == 0 || !type) return true;
    Runtime::Resource *inst = rt->instantiate(type, parent, value_string(a, 4),
                                              value_string(a, 3), false, err);
    if (!inst) return false;
    rt->bind(h, inst);
    return true;
}

// 0x4dbbe0: FUN_004ec210, resource-tree reparent.
bool gr_set_parent(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setParent", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setParent", a, &o, err)) return false;
    if (!want("GameRef.setParent", a, 2, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) game_rt()->reparent(r, value_resource(a[1]));
    *out = Value::make_null();
    return true;
}

// 0x4dbd00: position from a Vector3.
bool gr_set_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setPos", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.setPos", a, &o, err) ||
        !want("GameRef.setPos", a, 2, err) ||
        !get_o("GameRef.setPos", a[1], &v, err))
        return false;
    float pos[3];
    if (!vec_read(vm, v, pos, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r)
        for (int i = 0; i < 3; i++) r->pos[i] = pos[i];
    *out = Value::make_null();
    return true;
}

// 0x4db950: pos + ori; null arguments default to zeros (FUN_004eb3f0).
bool gr_set_matrix(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setMatrix", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setMatrix", a, &o, err) ||
        !want("GameRef.setMatrix", a, 3, err))
        return false;
    float pos[3], ori[3];
    if (!read_vec_arg(vm, "GameRef.setMatrix", a[1], pos, err) ||
        !read_ypr_arg(vm, "GameRef.setMatrix", a[2], ori, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) {
        for (int i = 0; i < 3; i++) {
            r->pos[i] = pos[i];
            r->ori[i] = ori[i];
        }
    }
    *out = Value::make_null();
    return true;
}

// 0x4dbf20: position, orientation, linear and angular velocity (12 floats).
bool gr_set_state(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setState", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setState", a, &o, err) ||
        !want("GameRef.setState", a, 5, err))
        return false;
    float pos[3], ori[3], vel[3], ang[3];
    if (!read_vec_arg(vm, "GameRef.setState", a[1], pos, err) ||
        !read_ypr_arg(vm, "GameRef.setState", a[2], ori, err) ||
        !read_vec_arg(vm, "GameRef.setState", a[3], vel, err) ||
        !read_vec_arg(vm, "GameRef.setState", a[4], ang, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) {
        for (int i = 0; i < 3; i++) {
            r->pos[i] = pos[i];
            r->ori[i] = ori[i];
            r->vel[i] = vel[i];
            r->ang[i] = ang[i];
        }
    }
    *out = Value::make_null();
    return true;
}

// 0x4dc510: typeID 0 uses the receiver's own type; otherwise the id is
// resolved as a game type. The receiver handle ends up on the new instance.
// shortcut: the exe's default ground/container (RI+0x1d4) is not modeled, so a
// null parent yields a parentless instance.
bool gt_create_native_instance(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.createNativeInstance", err)) return false;
    Object *o;
    if (!receiver_object("GameType.createNativeInstance", a, &o, err)) return false;
    int32_t type_id;
    if (!want("GameType.createNativeInstance", a, 5, err) ||
        !get_i("GameType.createNativeInstance", a[2], &type_id, err))
        return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);
    *out = Value::make_null();
    if (!h) return true;
    Runtime::Resource *type =
        type_id ? rt->resolve((uint32_t)type_id) : h->target;
    if (!type) return true;
    Runtime::Resource *inst =
        rt->instantiate(type, value_resource(a[1]), value_string(a, 4),
                        value_string(a, 3), false, err);
    if (!inst) return false;
    rt->bind(h, inst);
    return true;
}

}  // namespace

namespace natives {

const std::vector<Entry> &table()
{
    static const std::vector<Entry> rows = {
    {"java.game.parts.bodypart.Chassis", "getAngvel", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMaterialIndex", "()I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMaxSteer", "()F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setMaxSteer", "(F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getShifting", "()I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getNitroing", "()I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getEngineTemp", "()F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getAIparam_float", "(I)F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getAIparam_int", "(I)I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setAIparam_float", "(IF)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setAIparam_int", "(II)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getRPM", "()F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setRPM", "(F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getTorque", "(FF)F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setTorque", "(F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMass", "()F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getCM", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMin", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMax", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.Vehicle", "getHorn", "()I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setHornSFX", "(Ljava.util.resource.ResourceRef;FI)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setNitroSFX", "(Ljava.util.resource.ResourceRef;F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setCooling", "(FFF)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "forceUpdate", "()V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMileage", "()F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setMileage", "(F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setBuck", "(IIFFFF)I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setSfxExhaustMinVol", "(F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setSteerWheelRadius", "(F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setSteerWheel", "(FF)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setAckermann", "(F)V", nullptr, true},
    {"java.game.Vehicle", "hasCrime", "()F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getSfxTable", "(I)Ljava.game.parts.SfxTable;", nullptr, true},
    {"java.game.parts.SfxTable", "getItems", "()I", nullptr, true},
    {"java.game.parts.SfxTable", "clear", "()V", nullptr, true},
    {"java.game.parts.SfxTable", "addItem", "(Ljava.util.resource.ResourceRef;FFFFF)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getWheels", "()I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getWheel", "(I)Ljava.game.parts.WheelRef;", nullptr, true},
    {"java.game.parts.WheelRef", "getPos", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.WheelRef", "getYpr", "()Ljava.lang.Ypr;", nullptr, true},
    {"java.game.parts.WheelRef", "getDrive", "()F", nullptr, true},
    {"java.game.parts.WheelRef", "getSteer", "()F", nullptr, true},
    {"java.game.parts.WheelRef", "getRadius", "()F", nullptr, true},
    {"java.game.parts.WheelRef", "getBrake", "()F", nullptr, true},
    {"java.game.parts.WheelRef", "getHBrake", "()F", nullptr, true},
    {"java.game.parts.WheelRef", "setPos", "(Ljava.lang.Vector3;)V", nullptr, true},
    {"java.game.parts.WheelRef", "setYpr", "(Ljava.lang.Ypr;)V", nullptr, true},
    {"java.game.parts.WheelRef", "setDrive", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setSteer", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setRadius", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setWidth", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setCPatch", "(FFF)V", nullptr, true},
    {"java.game.parts.WheelRef", "setFriction", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setFrictn_x", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setSliction", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setStiffness", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setRollRes", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setBearing", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setMaxLoad", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setLoadSmooth", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setPacejka", "(IF)V", nullptr, true},
    {"java.game.parts.WheelRef", "setForce", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setDamping", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setDamping", "(FF)V", nullptr, true},
    {"java.game.parts.WheelRef", "setRestLen", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setMinLen", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setMaxLen", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setBrake", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setHBrake", "(F)V", nullptr, true},
    {"java.game.parts.WheelRef", "setArm", "(FFFFFFF)V", nullptr, true},
    {"java.game.parts.WheelRef", "setHub", "(FFFFFFFFFF)V", nullptr, true},
    {"java.game.parts.WheelRef", "setInstantCenter", "(FFFFFF)V", nullptr, true},
    {"java.game.parts.WheelRef", "setOppWheel", "(I)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getWheelPos", "(I)Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getWheelDamage", "(I)Ljava.lang.String;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setWheelDamage", "(ILjava.lang.String;)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setOSD", "(IF)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getOSD", "(I)F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getNitro", "()F", nullptr, true},
    {"java.game.parts.Part", "getSlots", "()I", nullptr, true},
    {"java.game.parts.Part", "getSlotID", "(I)I", nullptr, true},
    {"java.game.parts.Part", "getSlotIndex", "(I)I", nullptr, true},
    {"java.game.parts.Part", "partOnSlot", "(I)Ljava.game.parts.Part;", nullptr, true},
    {"java.game.parts.Part", "slotIDOnSlot", "(I)I", nullptr, true},
    {"java.game.parts.Part", "getSlotDamage", "(I)Ljava.lang.String;", nullptr, true},
    {"java.game.parts.Part", "setSlotDamage", "(ILjava.lang.String;)V", nullptr, true},
    {"java.game.parts.Part", "setSlotPos", "(ILjava.lang.Vector3;Ljava.lang.Ypr;)V", nullptr, true},
    {"java.game.parts.Part", "getCarRef", "()Ljava.game.parts.Part;", nullptr, true},
    {"java.game.parts.Part", "getWear", "()F", nullptr, true},
    {"java.game.parts.Part", "setWear", "(F)F", nullptr, true},
    {"java.game.parts.Part", "setMaxWear", "(F)V", nullptr, true},
    {"java.game.parts.Part", "getTear", "()F", nullptr, true},
    {"java.game.parts.Part", "setTear", "(F)F", nullptr, true},
    {"java.game.parts.Part", "disableSlot", "(II)V", nullptr, true},
    {"java.game.parts.Part", "isSlotDisabled", "(I)I", nullptr, true},
    {"java.game.parts.Part", "getTexture", "()I", nullptr, true},
    {"java.game.parts.Part", "setTexture", "(I)I", nullptr, true},
    {"java.game.parts.Part", "getMesh", "()I", nullptr, true},
    {"java.game.parts.Part", "setMesh", "(I)I", nullptr, true},
    {"java.game.parts.Part", "getRenderType", "()I", nullptr, true},
    {"java.game.parts.Part", "setRenderType", "(I)I", nullptr, true},
    {"java.game.parts.Part", "getLogo", "()I", nullptr, true},
    {"java.game.parts.Part", "getCar", "()I", nullptr, true},
    {"java.game.parts.Part", "getWheelID", "()I", nullptr, true},
    {"java.game.parts.Part", "getMass", "()F", nullptr, true},
    {"java.game.parts.Part", "setSfxLoopParams", "(FF)I", nullptr, true},
    {"java.game.parts.Part", "flap", "(I)I", nullptr, true},
    {"java.game.parts.Part", "install_OK", "(Ljava.util.resource.GameRef;ILjava.util.resource.GameRef;ILjava.lang.Vector3;)[I", nullptr, true},
    {"java.game.parts.DynoData", "newNative", "()V", nullptr, true},
    {"java.game.parts.DynoData", "deleteNative", "()V", nullptr, true},
    {"java.game.parts.DynoData", "calcDyno", "(F)F", nullptr, true},
    {"java.game.parts.DynoData", "getTorque", "(FF)F", nullptr, true},
    {"java.game.parts.DynoData", "getHP", "(FF)F", nullptr, true},
    {"java.io.Controller", "user_Add", "(IIIFFFF)I", nullptr, true},
    {"java.io.Controller", "user_Del", "(III)I", nullptr, true},
    {"java.io.Controller", "user_SetAxisSmooth", "(IFFFFF)I", nullptr, true},
    {"java.io.Controller", "user_SetAxisSpeed", "(IF)V", nullptr, true},
    {"java.io.Controller", "user_Reset", "()V", nullptr, true},
    {"java.io.Controller", "user_GetAxisVal", "(I)F", nullptr, true},
    {"java.io.Controller", "user_SetAxisForce", "(IFF)V", nullptr, true},
    {"java.game.GameLogic", "kismajomCheck", "([Ljava.lang.String;)I", nullptr, true},
    {"java.lang.System", "exit", "()V", nullptr, true},
    {"java.lang.System", "exit", "(Ljava.lang.String;)V", noop_void, false},
    {"java.lang.System", "log", "(Ljava.lang.String;)V", nullptr, true},
    {"java.lang.System", "gc", "()I", nullptr, true},
    {"java.lang.System", "runFinalization", "()I", zero_int, false},
    {"java.lang.System", "analyze", "(Ljava.lang.Object;)V", noop_void, false},
    {"java.lang.System", "analyze", "(I)V", noop_void, false},
    {"java.lang.System", "analyze", "(F)V", noop_void, false},
    {"java.lang.System", "arraycopy", "(Ljava.lang.Object;ILjava.lang.Object;II)V", system_arraycopy, false},
    {"java.lang.System", "currentTime", "()F", system_current_time, false},
    {"java.lang.System", "simTime", "()F", nullptr, true},
    {"java.lang.System", "timeWarp", "(F)F", nullptr, true},
    {"java.lang.System", "openLib", "(Ljava.lang.String;)I", nullptr, true},
    {"java.lang.System", "compileAll", "(Ljava.lang.String;)I", nullptr, true},
    {"java.lang.System", "info", "(I)Ljava.lang.String;", nullptr, true},
    {"java.lang.System", "buildNumber", "()I", system_build_number, false},
    {"java.lang.System", "setLdPriority", "(I)V", nullptr, true},
    {"java.lang.System", "syncGameTime", "(F)V", nullptr, true},
    {"java.lang.System", "setMeasure", "(F)V", nullptr, true},
    {"java.lang.System", "getConfigOptions", "()V", nullptr, true},
    {"java.lang.System", "isLoadingReset", "()V", nullptr, true},
    {"java.lang.System", "isLoading", "()I", nullptr, true},
    {"java.lang.System", "memUsage", "()I", nullptr, true},
    {"java.lang.Float", "toString", "(FLjava.lang.String;)Ljava.lang.String;", float_tostring, false},
    {"java.lang.Integer", "toString", "(I)Ljava.lang.String;", int_tostring, false},
    {"java.lang.Integer", "toHexString", "(I)Ljava.lang.String;", int_tohexstring, false},
    {"java.lang.String", "finalize", "()V", string_finalize, false},
    {"java.lang.String", "token", "(ILjava.lang.String;)Ljava.lang.String;", string_token, false},
    {"java.lang.String", "intValue", "()I", string_intvalue, false},
    {"java.lang.String", "floatValue", "()F", string_floatvalue, false},
    {"java.lang.String", "append", "(I)Ljava.lang.String;", string_append, false},
    {"java.lang.String", "length", "()I", string_length, false},
    {"java.lang.String", "getASCII", "()[I", string_getascii, false},
    {"java.lang.Vector3", "rotate", "(Ljava.lang.Ypr;)Ljava.lang.Vector3;", vec_rotate_ypr, false},
    {"java.lang.Vector3", "length", "()F", vec_length, false},
    {"java.lang.Vector3", "distance", "(Ljava.lang.Vector3;)F", vec_distance, false},
    {"java.lang.Vector3", "normalize", "()V", vec_normalize, false},
    {"java.lang.Vector3", "rotate", "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;", vec_rotate_axis, false},
    {"java.lang.Vector3", "<init>", "(Ljava.lang.Ypr;)", vec_ctor_ypr, false},
    {"java.lang.Ypr", "<init>", "(Ljava.lang.Vector3;)", ypr_ctor_vector, false},
    {"java.lang.Ypr", "mul", "(Ljava.lang.Ypr;)V", ypr_mul, false},
    {"java.lang.Thread", "init", "(Ljava.lang.String;)V", nullptr, true},
    {"java.lang.Thread", "start", "()V", nullptr, true},
    {"java.lang.Thread", "stop", "()V", nullptr, true},
    {"java.lang.Thread", "isAlive", "()I", nullptr, true},
    {"java.lang.Thread", "getPriority", "()I", nullptr, true},
    {"java.lang.Thread", "setPriority", "(I)V", nullptr, true},
    {"java.lang.Thread", "sleep", "(F)V", nullptr, true},
    {"java.lang.Thread", "suspend", "()V", nullptr, true},
    {"java.lang.Thread", "resume", "()V", nullptr, true},
    {"java.lang.Thread", "setDaemon", "(I)V", nullptr, true},
    {"java.lang.Object", "wait", "()V", nullptr, true},
    {"java.lang.Object", "notify", "()V", nullptr, true},
    {"java.lang.Object", "notifyAll", "()V", nullptr, true},
    {"java.lang.Object", "toString", "()Ljava.lang.String;", nullptr, true},
    {"java.lang.Object", "disableGC", "()V", object_gc_toggle, false},
    {"java.lang.Object", "enableGC", "()V", object_gc_toggle, false},
    {"java.lang.Object", "hashCode", "()I", object_hashcode, false},
    {"java.lang.Math", "random", "()F", math_random, false},
    {"java.lang.Math", "sqrt", "(F)F", math_sqrt, false},
    {"java.lang.Math", "randomize", "()V", math_randomize, false},
    {"java.lang.Math", "setrandseed", "(I)V", math_setrandseed, false},
    {"java.lang.Math", "ln", "(F)F", math_ln, false},
    {"java.lang.Math", "log10", "(F)F", math_ln, false},
    {"java.lang.Math", "exp", "(F)F", math_exp, false},
    {"java.lang.Math", "pow", "(FF)F", math_pow, false},
    {"java.lang.Math", "atan2", "(FF)F", math_atan2, false},
    {"java.io.Input", "getAxis", "(II)F", nullptr, true},
    {"java.io.Input", "getDeviceName", "(I)Ljava.lang.String;", nullptr, true},
    {"java.io.Input", "mapAxis", "(Ljava.util.resource.GameRef;IIIFFFF)V", nullptr, true},
    {"java.io.Input", "setAxisSmooth", "(Ljava.util.resource.GameRef;IFFFF)V", nullptr, true},
    {"java.io.Input", "activeAxis", "(I)I", nullptr, true},
    {"java.io.Input", "axisName", "(II)Ljava.lang.String;", nullptr, true},
    {"java.io.Input", "lastKey", "()I", nullptr, true},
    {"java.io.Input", "createHotkey", "(IILjava.io.Hotkey;Ljava.util.resource.GameRef;Ljava.render.Osd;I)V", nullptr, true},
    {"java.io.Input", "deleteHotkey", "(Ljava.io.Hotkey;)V", nullptr, true},
    {"java.io.Input", "checkHotkeys", "(Ljava.util.resource.GameRef;Ljava.render.Osd;)V", nullptr, true},
    {"java.io.Input", "flushHotkeys", "()V", nullptr, true},
    {"java.io.MouseCursor", "getPickedPos", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.io.MouseCursor", "getPos", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.io.MouseCursor", "getPickedPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", nullptr, true},
    {"java.io.MouseCursor", "getPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", nullptr, true},
    {"java.io.File", "open", "(I)I", nullptr, true},
    {"java.io.File", "close", "()V", nullptr, true},
    {"java.io.File", "delete", "(Ljava.lang.String;)I", nullptr, true},
    {"java.io.File", "copy", "(Ljava.lang.String;Ljava.lang.String;)I", nullptr, true},
    {"java.io.File", "move", "(Ljava.lang.String;Ljava.lang.String;)I", nullptr, true},
    {"java.io.File", "exists", "(Ljava.lang.String;)I", nullptr, true},
    {"java.io.File", "write", "(I)I", nullptr, true},
    {"java.io.File", "write", "(F)I", nullptr, true},
    {"java.io.File", "write", "(Ljava.util.resource.ResourceRef;)I", nullptr, true},
    {"java.io.File", "write", "(Ljava.lang.String;)I", nullptr, true},
    {"java.io.File", "readInt", "()I", nullptr, true},
    {"java.io.File", "readFloat", "()F", nullptr, true},
    {"java.io.File", "readResID", "()I", nullptr, true},
    {"java.io.File", "readString", "()Ljava.lang.String;", nullptr, true},
    {"java.io.FindFile", "first", "(Ljava.lang.String;I)Ljava.lang.String;", nullptr, true},
    {"java.io.FindFile", "next", "()Ljava.lang.String;", nullptr, true},
    {"java.io.FindFile", "close", "()V", nullptr, true},
    {"java.sound.Sound", "changeMusicSet", "(I)V", nullptr, true},
    {"java.sound.Sound", "nextTrack", "()V", nullptr, true},
    {"java.sound.Sound", "prevTrack", "()V", nullptr, true},
    {"java.sound.Sound", "firstTrack", "()V", nullptr, true},
    {"java.sound.Sound", "randomTrack", "()V", nullptr, true},
    {"java.sound.Sound", "playTrack", "(I)V", nullptr, true},
    {"java.sound.Sound", "playTrack", "(Ljava.lang.String;)V", nullptr, true},
    {"java.sound.Sound", "getTotalTracks", "()I", nullptr, true},
    {"java.sound.Sound", "getNowPlaying", "()I", nullptr, true},
    {"java.sound.Sound", "stopMusic", "()V", nullptr, true},
    {"java.sound.Sound", "getMode", "()I", nullptr, true},
    {"java.sound.Sound", "setMode", "(I)I", nullptr, true},
    {"java.sound.Sound", "setVolume", "(IF)V", nullptr, true},
    {"java.sound.Sound", "getVolume", "(I)F", nullptr, true},
    {"java.sound.Sound", "has3DHardware", "()I", nullptr, true},
    {"java.sound.Sound", "hasMixHardware", "()I", nullptr, true},
    {"java.sound.Sound", "enableDebugDump", "(Ljava.util.resource.ResourceRef;)I", nullptr, true},
    {"java.render.GfxEngine", "forceRendering", "()V", nullptr, true},
    {"java.render.GfxEngine", "printScreen", "(Ljava.lang.String;)V", nullptr, true},
    {"java.render.GfxEngine", "numDisplayModes", "()I", nullptr, true},
    {"java.render.GfxEngine", "currDisplayMode", "()I", nullptr, true},
    {"java.render.GfxEngine", "displayModeName", "(I)Ljava.lang.String;", nullptr, true},
    {"java.render.GfxEngine", "changeVideoMode", "(III)V", nullptr, true},
    {"java.render.GfxEngine", "setGlobalEnvmap", "(Ljava.util.resource.ResourceRef;)V", nullptr, true},
    {"java.render.GfxEngine", "flush", "()V", nullptr, true},
    {"java.render.GfxEngine", "renderOK", "()I", nullptr, true},
    {"java.render.GfxEngine", "openVideo", "(Ljava.lang.String;II)I", nullptr, true},
    {"java.render.GfxEngine", "openVideo", "(Ljava.lang.String;III)I", nullptr, true},
    {"java.render.GfxEngine", "closeVideo", "()V", nullptr, true},
    {"java.render.GfxEngine", "isPlayingVideo", "()I", nullptr, true},
    {"java.render.Camera", "create", "(Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V", nullptr, true},
    {"java.render.Camera", "destroy", "()V", nullptr, true},
    {"java.render.Camera", "setFog", "(IFF)V", nullptr, true},
    {"java.render.Viewport", "create", "(IFFFF)V", nullptr, true},
    {"java.render.Viewport", "destroy", "()V", nullptr, true},
    {"java.render.Viewport", "activate", "(I)V", nullptr, true},
    {"java.render.Viewport", "deactivate", "()V", nullptr, true},
    {"java.render.Viewport", "getAspect", "()F", nullptr, true},
    {"java.render.Viewport", "getWidth", "()F", nullptr, true},
    {"java.render.Viewport", "getHeight", "()F", nullptr, true},
    {"java.render.Viewport", "getTop", "()F", nullptr, true},
    {"java.render.Viewport", "getLeft", "()F", nullptr, true},
    {"java.render.Viewport", "unproject", "(Ljava.lang.Vector3;I)Ljava.lang.Vector3;", nullptr, true},
    {"java.render.Viewport", "resize", "(FFFF)V", nullptr, true},
    {"java.render.Text", "setScale", "(FF)V", nullptr, true},
    {"java.render.Text", "setDefaultColor", "(I)V", nullptr, true},
    {"java.render.Text", "getDefaultColor", "()I", nullptr, true},
    {"java.render.Text", "create", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;FF)V", nullptr, true},
    {"java.render.Text", "changeColor", "(I)V", nullptr, true},
    {"java.render.Text", "changeAlign", "(I)V", nullptr, true},
    {"java.render.Text", "setPos", "(FF)V", nullptr, true},
    {"java.render.Text", "getPos", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.render.Text", "update", "()V", nullptr, true},
    {"java.render.Text", "getWidthPixels", "(Ljava.lang.String;Ljava.util.resource.ResourceRef;)F", nullptr, true},
    {"java.render.Text", "getPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", nullptr, true},
    {"java.util.resource.Animation", "<init>", "(Ljava.util.resource.RenderRef;Ljava.util.resource.ResourceRef;)", nullptr, true},
    {"java.util.resource.Animation", "finalize", "()V", nullptr, true},
    {"java.util.resource.Animation", "setSpeed", "(F)V", nullptr, true},
    {"java.util.resource.Animation", "setFade", "(F)V", nullptr, true},
    {"java.util.resource.Animation", "getPos", "()F", nullptr, true},
    {"java.util.resource.Animation", "seek", "(F)V", nullptr, true},
    {"java.util.resource.Animation", "play", "()V", nullptr, true},
    {"java.util.resource.Animation", "loopPlay", "()V", nullptr, true},
    {"java.util.resource.Animation", "pause", "()V", nullptr, true},
    {"java.util.resource.ParticleSystem", "init", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V", nullptr, true},
    {"java.util.resource.ParticleSystem", "stop", "()V", nullptr, true},
    {"java.util.resource.ParticleSystem", "setFreq", "(F)V", nullptr, true},
    {"java.util.resource.ParticleSystem", "setDirectSource", "(Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V", nullptr, true},
    {"java.util.resource.ParticleSystem", "setSource", "(Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V", nullptr, true},
    {"java.util.resource.ParticleSystem", "delAction", "(Ljava.lang.String;)V", nullptr, true},
    {"java.util.resource.ParticleSystem", "setCounter", "(Ljava.lang.String;Ljava.lang.Vector3;F)V", nullptr, true},
    {"java.util.resource.ParticleSystem", "getCounter", "(Ljava.lang.String;)I", nullptr, true},
    {"java.util.resource.ParticleSystem", "modePermanent", "(I)V", nullptr, true},
    {"java.lang.GameType", "remNotification", "(Ljava.util.resource.GameRef;I)V", nullptr, true},
    {"java.lang.GameType", "addNotification", "(Ljava.util.resource.GameRef;IILjava.lang.String;)V", nullptr, true},
    {"java.lang.GameType", "addNotification", "(Ljava.util.resource.GameRef;IILjava.lang.String;Ljava.lang.String;)V", nullptr, true},
    {"java.lang.GameType", "addTimer", "(FI)V", nullptr, true},
    {"java.lang.GameType", "removeAllTimers", "()V", nullptr, true},
    {"java.lang.GameType", "setEventMask", "(I)V", nullptr, true},
    {"java.lang.GameType", "clearEventMask", "(I)V", nullptr, true},
    {"java.lang.GameType", "registerCallback", "(I)V", nullptr, true},
    {"java.lang.GameType", "unregisterCallback", "(I)V", nullptr, true},
    {"java.lang.GameType", "unregisterCallbacks", "()V", nullptr, true},
    {"java.lang.GameType", "createNativeInstance", "(Ljava.util.resource.GameRef;ILjava.lang.String;Ljava.lang.String;)V", gt_create_native_instance, true},
    {"java.util.resource.ResourceRef", "deleteNative", "()V", rr_delete_native, true},
    {"java.util.resource.ResourceRef", "newNative", "()V", rr_new_native, true},
    {"java.util.resource.ResourceRef", "set", "(I)V", rr_set_id, true},
    {"java.util.resource.ResourceRef", "set", "(Ljava.util.resource.ResourceRef;)V", rr_set_ref, true},
    {"java.util.resource.ResourceRef", "load", "()V", nullptr, true},
    {"java.util.resource.ResourceRef", "unload", "()V", nullptr, true},
    {"java.util.resource.ResourceRef", "destroy", "()V", nullptr, true},
    {"java.util.resource.ResourceRef", "type", "()I", rr_type, true},
    {"java.util.resource.ResourceRef", "id", "()I", rr_id, true},
    {"java.util.resource.ResourceRef", "getParentID", "()I", rr_get_parent_id, true},
    {"java.util.resource.ResourceRef", "getParent", "()Ljava.util.resource.ResourceRef;", rr_get_parent, true},
    {"java.util.resource.ResourceRef", "getWTRoot", "()Ljava.util.resource.ResourceRef;", rr_get_wt_root, true},
    {"java.util.resource.ResourceRef", "getFirstChild", "()Ljava.util.resource.ResourceRef;", rr_get_first_child, true},
    {"java.util.resource.ResourceRef", "getNextChild", "()Ljava.util.resource.ResourceRef;", rr_get_next_child, true},
    {"java.util.resource.ResourceRef", "makeTexture", "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V", nullptr, true},
    {"java.util.resource.ResourceRef", "makeSound", "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V", nullptr, true},
    {"java.util.resource.ResourceRef", "cache", "()V", nullptr, true},
    {"java.util.resource.ResourceRef", "precache", "()V", nullptr, true},
    {"java.util.resource.ResourceRef", "duplicate", "(Ljava.util.resource.ResourceRef;)V", nullptr, true},
    {"java.util.resource.ResourceRef", "scaleMesh", "(FFF)V", nullptr, true},
    {"java.util.resource.GameRef", "create", "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.String;Ljava.lang.String;)Ljava.lang.GameType;", gr_create, true},
    {"java.util.resource.GameRef", "create_native", "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.String;Ljava.lang.String;)V", gr_create_native, true},
    {"java.util.resource.GameRef", "getFlags", "()I", gr_get_flags, true},
    {"java.util.resource.GameRef", "setFlags", "(I)V", gr_set_flags, true},
    {"java.util.resource.GameRef", "clearFlags", "(I)V", gr_clear_flags, true},
    {"java.util.resource.GameRef", "getPos", "()Ljava.lang.Vector3;", gr_get_pos, true},
    {"java.util.resource.GameRef", "getOri", "()Ljava.lang.Ypr;", gr_get_ori, true},
    {"java.util.resource.GameRef", "getVel", "()Ljava.lang.Vector3;", gr_get_vel, true},
    {"java.util.resource.GameRef", "getInfo", "(II)I", nullptr, true},
    {"java.util.resource.GameRef", "getInfo", "(ILjava.lang.String;)I", nullptr, true},
    {"java.util.resource.GameRef", "queueEvent", "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V", nullptr, true},
    {"java.util.resource.GameRef", "setParent", "(Ljava.util.resource.ResourceRef;)V", gr_set_parent, true},
    {"java.util.resource.GameRef", "setPos", "(Ljava.lang.Vector3;)V", gr_set_pos, true},
    {"java.util.resource.GameRef", "setMatrix", "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", gr_set_matrix, true},
    {"java.util.resource.GameRef", "setState", "(Ljava.lang.Vector3;Ljava.lang.Ypr;Ljava.lang.Vector3;Ljava.lang.Vector3;)V", gr_set_state, true},
    {"java.util.resource.GameRef", "isEmpty", "()I", gr_is_empty, true},
    {"java.util.resource.GameRef", "isScripted", "(Ljava.lang.String;)I", gr_is_scripted, true},
    {"java.util.resource.GameRef", "getScriptInstance", "()Ljava.lang.Object;", gr_get_script_instance, true},
    {"java.util.resource.GameRef", "setActiveCollision", "()V", nullptr, true},
    {"java.util.resource.GameRef", "getDetail", "()F", gr_get_detail, true},
    {"java.util.resource.GameRef", "getPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", gr_get_pos_out, true},
    {"java.util.resource.GameRef", "getOri", "(Ljava.lang.Ypr;)Ljava.lang.Ypr;", gr_get_ori_out, true},
    {"java.util.resource.GameRef", "getVel", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", gr_get_vel_out, true},
    {"java.util.resource.GroundRef", "setWater", "(FFF)V", nullptr, true},
    {"java.util.resource.GroundRef", "setWater", "(Ljava.lang.Vector3;Ljava.lang.Vector3;FF)V", nullptr, true},
    {"java.util.resource.GroundRef", "addWaterLimit", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)V", nullptr, true},
    {"java.util.resource.GroundRef", "setFog", "(IFF)V", nullptr, true},
    {"java.util.resource.GroundRef", "addTrafficCar", "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I", nullptr, true},
    {"java.util.resource.GroundRef", "remTrafficCar", "(I)V", nullptr, true},
    {"java.util.resource.GroundRef", "notifyTrafficCar", "(II)I", nullptr, true},
    {"java.util.resource.GroundRef", "addTrafficN", "(Ljava.util.resource.GameRef;IFFF)I", nullptr, true},
    {"java.util.resource.GroundRef", "addTrafficP", "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I", nullptr, true},
    {"java.util.resource.GroundRef", "delTraffic", "()V", nullptr, true},
    {"java.util.resource.GroundRef", "setTrafficCarBehaviour", "(II)V", nullptr, true},
    {"java.util.resource.GroundRef", "haltTrafficCross", "(Ljava.lang.Vector3;F)V", nullptr, true},
    {"java.util.resource.GroundRef", "haltTrafficPath", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)V", nullptr, true},
    {"java.util.resource.GroundRef", "haltTrafficRoad", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)V", nullptr, true},
    {"java.util.resource.GroundRef", "clearHalts", "()V", nullptr, true},
    {"java.util.resource.GroundRef", "setTrafficCrossStopOffset", "(F)V", nullptr, true},
    {"java.util.resource.GroundRef", "getNearestCross", "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;", nullptr, true},
    {"java.util.resource.GroundRef", "getStartDirection", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)Ljava.lang.Vector3;", nullptr, true},
    {"java.util.resource.GroundRef", "getRouteLength", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F", nullptr, true},
    {"java.util.resource.GroundRef", "alignToRoad", "(Ljava.lang.Vector3;)[Ljava.lang.Vector3;", nullptr, true},
    {"java.util.resource.GroundRef", "findRoute", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F", nullptr, true},
    {"java.util.resource.GroundRef", "getRouteLength", "()F", nullptr, true},
    {"java.util.resource.GroundRef", "getRoutePos", "(F)Ljava.lang.Vector3;", nullptr, true},
    {"java.util.resource.GroundRef", "getRouteDist", "(Ljava.lang.Vector3;)F", nullptr, true},
    {"java.util.resource.GroundRef", "setPedestrianDensityN", "(F)V", nullptr, true},
    {"java.util.resource.GroundRef", "addPedestrianType", "(Ljava.util.resource.GameRef;)V", nullptr, true},
    {"java.util.resource.GroundRef", "removePedestrianType", "(Ljava.util.resource.GameRef;)V", nullptr, true},
    {"java.util.resource.GroundRef", "pedestrianDistance", "(Ljava.lang.Vector3;I)F", nullptr, true},
    {"java.util.resource.GroundRef", "getTrafficCarPos", "(ILjava.lang.Vector3;)V", nullptr, true},
    {"java.util.resource.GroundRef", "getPedestrianPos", "(Ljava.lang.Vector3;Ljava.lang.Vector3;I)V", nullptr, true},
    {"java.util.resource.RenderRef", "create", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V", nullptr, true},
    {"java.util.resource.RenderRef", "getBoneId", "(Ljava.lang.String;)I", nullptr, true},
    {"java.util.resource.RenderRef", "setMatrix", "(ILjava.util.resource.ResourceRef;Ljava.lang.Vector3;Ljava.lang.Ypr;)V", nullptr, true},
    {"java.util.resource.RenderRef", "setMatrix", "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", nullptr, true},
    {"java.util.resource.RenderRef", "changeResource", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)V", nullptr, true},
    {"java.util.resource.RenderRef", "setColor", "(I)V", nullptr, true},
    {"java.util.resource.RenderRef", "setLight", "(III)V", nullptr, true},
    {"java.util.resource.RenderRef", "setFlare", "(Ljava.util.resource.ResourceRef;IFFII)V", nullptr, true},
    {"java.util.resource.RenderRef", "setType", "(Ljava.util.resource.ResourceRef;)V", nullptr, true},
    {"java.util.resource.RenderRef", "getTypeID", "()I", nullptr, true},
    {"java.util.resource.RenderRef", "getDetail", "()F", nullptr, true},
    {"java.util.resource.RenderRef", "lineCreate", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)I", nullptr, true},
    {"java.util.resource.RenderRef", "lineAdd", "(Ljava.lang.Vector3;Ljava.lang.Vector3;IF)V", nullptr, true},
    {"java.util.resource.RenderRef", "plotRoute", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;IFLjava.lang.Vector3;)I", nullptr, true},
    {"java.util.resource.PhysicsRef", "create", "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V", nullptr, true},
    {"java.util.resource.PhysicsRef", "createBox", "(Ljava.util.resource.ResourceRef;FFFLjava.lang.String;)V", nullptr, true},
    {"java.util.resource.PhysicsRef", "createSphere", "(Ljava.util.resource.ResourceRef;FLjava.lang.String;)V", nullptr, true},
    {"java.util.resource.PhysicsRef", "setMatrix", "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", nullptr, true},
    {"java.util.resource.PhysicsRef", "getPos", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.util.resource.PhysicsRef", "getOri", "()Ljava.lang.Ypr;", nullptr, true},
    {"java.util.resource.SfxRef", "nplay", "(Ljava.lang.Vector3;FFFII)I", nullptr, true},
    {"java.util.resource.SfxRef", "stop", "(I)V", nullptr, true},
    {"java.game.Painter", "paintPart", "(Ljava.util.resource.GameRef;I)V", nullptr, true},
    {"java.game.Painter", "doPaint", "(Ljava.util.resource.GameRef;IIIFFI)V", nullptr, true},
    {"java.game.Painter", "xPaint", "(Ljava.util.resource.GameRef;)V", nullptr, true},
    {"java.game.Navigator", "updateNavigator", "(Ljava.util.resource.GameRef;I)V", nullptr, true},
    {"java.game.Vehicle", "getSpeedSquare", "()F", nullptr, true},
    {"java.util.Replay", "writeFile", "(Ljava.lang.String;Ljava.lang.String;)V", nullptr, true},
    {"java.util.Replay", "readFile", "(ILjava.lang.String;Ljava.lang.String;)F", nullptr, true},
    {"java.lang.Debug", "writeFile", "(Ljava.lang.String;Ljava.lang.String;)V", nullptr, true},
    {"java.lang.Object", "errorVM", "(Ljava.lang.String;)V", nullptr, true},
    {"java.lang.Debug", "errorEX", "(Ljava.lang.String;)V", nullptr, true},
    {"java.lang.Debug", "giveWarning", "(Ljava.lang.String;)V", nullptr, true},
    {"java.lang.Object", "runDLL", "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;[Ljava.lang.String;)Ljava.lang.String;", nullptr, true},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;I)I", nullptr, true},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;F)I", nullptr, true},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;Ljava.lang.Object;)I", nullptr, true},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;)I", nullptr, true},
    {"java.net.NetworkEngine", "httpRequest", "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;)Ljava.lang.String;", nullptr, true},
    {"java.net.NetworkEngine", "httpRequestAsync", "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.util.resource.GameRef;Ljava.lang.String;)I", nullptr, true},
    {"java.util.Steam", "initAPI", "()I", nullptr, true},
    {"java.util.Steam", "neutralize", "()V", nullptr, true},
    {"java.util.Steam", "getPackage", "(I)Ljava.lang.String;", nullptr, true},
    {"java.util.Steam", "setAchievement", "(Ljava.lang.String;)V", nullptr, true},
    {"java.util.Steam", "resetAchievements", "()V", nullptr, true},
    };
    return rows;
}

void install(Vm &vm)
{
    for (const Entry &e : table()) {
        Vm::NativeFn fn = e.fn;
        if (!fn) {
            std::string cls = e.cls, method = e.method, sig = e.sig;
            fn = [cls, method, sig](Vm &, const std::vector<Value> &, Value *,
                                    std::string *err) {
                *err = "native not implemented (stub): " + cls + "." + method + sig;
                return false;
            };
        }
        vm.register_native(e.cls, e.method, e.sig, std::move(fn));
    }
}

}  // namespace natives
