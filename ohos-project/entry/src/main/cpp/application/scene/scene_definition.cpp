#include "scene_definition.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

extern "C" int OHOS_ReadRawFile(const char* path, void** data, size_t* size);
extern "C" void OHOS_FreeRawFile(void* data);

namespace scene {
namespace {

constexpr std::size_t kMaxJsonBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxEntities = 4096;
constexpr std::size_t kMaxStaticCubeRenderables = 256;
constexpr unsigned kMaxJsonDepth = 64;

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue* Find(const char* key) const
    {
        if (type != Type::Object) {
            return nullptr;
        }
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }
};

class JsonParser {
public:
    JsonParser(const char* data, std::size_t size) : data_(data), size_(size) {}

    bool Parse(JsonValue& root, std::string& error)
    {
        SkipWhitespace();
        if (!ParseValue(root, 0)) {
            error = error_;
            return false;
        }
        SkipWhitespace();
        if (position_ != size_) {
            Fail("trailing characters after JSON value");
            error = error_;
            return false;
        }
        return true;
    }

private:
    bool Fail(const char* reason)
    {
        if (error_.empty()) {
            error_ = reason;
            error_ += " at byte ";
            error_ += std::to_string(position_);
        }
        return false;
    }

    void SkipWhitespace()
    {
        while (position_ < size_) {
            const char value = data_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool Consume(char expected)
    {
        if (position_ >= size_ || data_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool ParseValue(JsonValue& value, unsigned depth)
    {
        if (depth > kMaxJsonDepth) {
            return Fail("JSON nesting limit exceeded");
        }
        SkipWhitespace();
        if (position_ >= size_) {
            return Fail("unexpected end of input");
        }
        switch (data_[position_]) {
        case '{':
            value.type = JsonValue::Type::Object;
            return ParseObject(value, depth + 1);
        case '[':
            value.type = JsonValue::Type::Array;
            return ParseArray(value, depth + 1);
        case '"':
            value.type = JsonValue::Type::String;
            return ParseString(value.string);
        case 't':
            return ParseLiteral("true", JsonValue::Type::Boolean, value, true);
        case 'f':
            return ParseLiteral("false", JsonValue::Type::Boolean, value, false);
        case 'n':
            return ParseLiteral("null", JsonValue::Type::Null, value, false);
        default:
            if (data_[position_] == '-' || (data_[position_] >= '0' && data_[position_] <= '9')) {
                return ParseNumber(value);
            }
            return Fail("unexpected token");
        }
    }

    bool ParseObject(JsonValue& value, unsigned depth)
    {
        ++position_; // '{'
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            SkipWhitespace();
            if (position_ >= size_ || data_[position_] != '"') {
                return Fail("object key must be a string");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return Fail("expected ':' after object key");
            }
            JsonValue member;
            if (!ParseValue(member, depth)) {
                return false;
            }
            if (!value.object.emplace(std::move(key), std::move(member)).second) {
                return Fail("duplicate object key");
            }
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return Fail("expected ',' or '}' in object");
            }
        }
    }

    bool ParseArray(JsonValue& value, unsigned depth)
    {
        ++position_; // '['
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            JsonValue item;
            if (!ParseValue(item, depth)) {
                return false;
            }
            value.array.push_back(std::move(item));
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return Fail("expected ',' or ']' in array");
            }
        }
    }

    static void AppendUtf8(std::uint32_t codePoint, std::string& output)
    {
        if (codePoint <= 0x7fu) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else if (codePoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    }

    bool ParseHex4(std::uint32_t& value)
    {
        if (size_ - position_ < 4) {
            return Fail("incomplete unicode escape");
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char digit = data_[position_++];
            value <<= 4;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                return Fail("invalid unicode escape");
            }
        }
        return true;
    }

    bool ParseString(std::string& output)
    {
        if (!Consume('"')) {
            return Fail("expected string");
        }
        while (position_ < size_) {
            const unsigned char value = static_cast<unsigned char>(data_[position_++]);
            if (value == '"') {
                return true;
            }
            if (value < 0x20u) {
                return Fail("unescaped control character in string");
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= size_) {
                return Fail("incomplete string escape");
            }
            const char escape = data_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t codePoint = 0;
                if (!ParseHex4(codePoint)) {
                    return false;
                }
                if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                    if (size_ - position_ < 6 || data_[position_] != '\\' ||
                        data_[position_ + 1] != 'u') {
                        return Fail("missing low surrogate");
                    }
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!ParseHex4(low) || low < 0xdc00u || low > 0xdfffu) {
                        return Fail("invalid low surrogate");
                    }
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10) + (low - 0xdc00u);
                } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                    return Fail("unexpected low surrogate");
                }
                AppendUtf8(codePoint, output);
                break;
            }
            default:
                return Fail("invalid string escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(JsonValue& value)
    {
        const std::size_t start = position_;
        Consume('-');
        if (position_ >= size_) {
            return Fail("incomplete number");
        }
        if (Consume('0')) {
            if (position_ < size_ && data_[position_] >= '0' && data_[position_] <= '9') {
                return Fail("leading zero in number");
            }
        } else {
            if (data_[position_] < '1' || data_[position_] > '9') {
                return Fail("invalid integer part");
            }
            while (position_ < size_ && data_[position_] >= '0' && data_[position_] <= '9') {
                ++position_;
            }
        }
        if (Consume('.')) {
            const std::size_t fractionStart = position_;
            while (position_ < size_ && data_[position_] >= '0' && data_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fractionStart) {
                return Fail("missing digits after decimal point");
            }
        }
        if (position_ < size_ && (data_[position_] == 'e' || data_[position_] == 'E')) {
            ++position_;
            if (position_ < size_ && (data_[position_] == '+' || data_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponentStart = position_;
            while (position_ < size_ && data_[position_] >= '0' && data_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponentStart) {
                return Fail("missing exponent digits");
            }
        }
        const std::string token(data_ + start, position_ - start);
        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0' || !std::isfinite(parsed)) {
            return Fail("number is not finite");
        }
        value.type = JsonValue::Type::Number;
        value.number = parsed;
        return true;
    }

    bool ParseLiteral(const char* literal, JsonValue::Type type, JsonValue& value, bool boolean)
    {
        const std::size_t length = std::strlen(literal);
        if (size_ - position_ < length || std::memcmp(data_ + position_, literal, length) != 0) {
            return Fail("invalid literal");
        }
        position_ += length;
        value.type = type;
        value.boolean = boolean;
        return true;
    }

    const char* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
    std::string error_;
};

const JsonValue* Member(const JsonValue& object, const char* key)
{
    return object.Find(key);
}

bool ReadFloat(const JsonValue* value, float& output)
{
    if (value == nullptr || value->type != JsonValue::Type::Number ||
        !std::isfinite(value->number) ||
        value->number < -std::numeric_limits<float>::max() ||
        value->number > std::numeric_limits<float>::max()) {
        return false;
    }
    output = static_cast<float>(value->number);
    return std::isfinite(output);
}

float FloatOr(const JsonValue& object, const char* key, float fallback)
{
    float value = fallback;
    ReadFloat(Member(object, key), value);
    return value;
}

int IntOr(const JsonValue& object, const char* key, int fallback)
{
    float value = static_cast<float>(fallback);
    if (!ReadFloat(Member(object, key), value) || value < static_cast<float>(std::numeric_limits<int>::min()) ||
        value > static_cast<float>(std::numeric_limits<int>::max())) {
        return fallback;
    }
    return static_cast<int>(value);
}

bool BoolOr(const JsonValue& object, const char* key, bool fallback)
{
    const JsonValue* value = Member(object, key);
    return value != nullptr && value->type == JsonValue::Type::Boolean ? value->boolean : fallback;
}

std::string StringOr(const JsonValue& object, const char* key, const std::string& fallback = {})
{
    const JsonValue* value = Member(object, key);
    return value != nullptr && value->type == JsonValue::Type::String ? value->string : fallback;
}

bool ReadVec3(const JsonValue* value, Vec3& output)
{
    if (value == nullptr || value->type != JsonValue::Type::Array || value->array.size() < 3) {
        return false;
    }
    return ReadFloat(&value->array[0], output.x) && ReadFloat(&value->array[1], output.y) &&
        ReadFloat(&value->array[2], output.z);
}

bool ReadQuaternion(const JsonValue* value, Quaternion& output)
{
    if (value == nullptr || value->type != JsonValue::Type::Array || value->array.size() < 4) {
        return false;
    }
    return ReadFloat(&value->array[0], output.w) && ReadFloat(&value->array[1], output.x) &&
        ReadFloat(&value->array[2], output.y) && ReadFloat(&value->array[3], output.z);
}

std::uint32_t UIntOr(const JsonValue* value, std::uint32_t fallback)
{
    if (value == nullptr || value->type != JsonValue::Type::Number ||
        !std::isfinite(value->number) || value->number < 0.0 ||
        value->number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(value->number) != value->number) {
        return fallback;
    }
    return static_cast<std::uint32_t>(value->number);
}

Transform ParseTransform(const JsonValue* value)
{
    Transform transform{};
    if (value == nullptr || value->type != JsonValue::Type::Object) {
        return transform;
    }
    ReadVec3(Member(*value, "position"), transform.position);
    ReadQuaternion(Member(*value, "rotation"), transform.rotation);
    ReadVec3(Member(*value, "scale"), transform.scale);
    return transform;
}

std::string NormalizePath(std::string path)
{
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        } else if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return path;
}

bool IsGroundSurface(const Entity& entity)
{
    if (entity.meshKind != MeshKind::UnitCube ||
        (entity.hasRigidBody && entity.rigidBodyType != 0)) {
        return false;
    }
    const float x = std::fabs(entity.transform.scale.x);
    const float y = std::fabs(entity.transform.scale.y);
    const float z = std::fabs(entity.transform.scale.z);
    // The Mikan prototype floor is represented as a broad, thin cube. Keep it
    // on the existing Jolt ground path instead of drawing a second floor with
    // a different height/extent.
    return x >= 8.0f && z >= 8.0f && y <= std::min(x, z) * 0.1f;
}

void ParseMaterial(const JsonValue* value, Material& material)
{
    if (value == nullptr || value->type != JsonValue::Type::Object) {
        return;
    }
    material.present = true;
    material.albedoPath = StringOr(*value, "albedoPath");
    material.normalPath = StringOr(*value, "normalPath");
    material.roughnessPath = StringOr(*value, "roughnessPath");
    material.metallicPath = StringOr(*value, "metallicPath");
    material.aoPath = StringOr(*value, "aoPath");
    material.emissivePath = StringOr(*value, "emissivePath");
    ReadVec3(Member(*value, "albedoColor"), material.albedoColor);
    material.metallic = FloatOr(*value, "metallic", material.metallic);
    material.roughness = FloatOr(*value, "roughness", material.roughness);
    material.ambientOcclusion = FloatOr(*value, "ao", material.ambientOcclusion);
    material.emissiveIntensity = FloatOr(*value, "emissiveIntensity", material.emissiveIntensity);
    material.useAlbedoTexture = BoolOr(*value, "useAlbedoTexture", false);
    material.useNormalTexture = BoolOr(*value, "useNormalTexture", false);
    material.useRoughnessTexture = BoolOr(*value, "useRoughnessTexture", false);
    material.useMetallicTexture = BoolOr(*value, "useMetallicTexture", false);
    material.useAoTexture = BoolOr(*value, "useAOTexture", false);
    material.useEmissiveTexture = BoolOr(*value, "useEmissiveTexture", false);
}

ColliderSettings ParseColliderSettings(const JsonValue& value, bool rigidBodyComponent)
{
    ColliderSettings settings{};
    settings.present = true;
    settings.shapeType = IntOr(value, rigidBodyComponent ? "shapeType" : "type", 0);
    ReadVec3(Member(value, "size"), settings.size);
    ReadVec3(Member(value, "offset"), settings.offset);
    settings.isTrigger = BoolOr(value, "isTrigger", false);
    settings.useOBB = BoolOr(value, "useOBB", false);
    settings.syncWithModel = BoolOr(value, "syncWithModel", true);
    settings.autoFitToModel = BoolOr(value, "autoFitToModel", false);
    return settings;
}

Quaternion NormalizeQuaternion(Quaternion rotation)
{
    const float length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y +
        rotation.z * rotation.z + rotation.w * rotation.w);
    if (!std::isfinite(length) || length < 1.0e-6f) {
        return Quaternion{};
    }
    const float inverseLength = 1.0f / length;
    rotation.x *= inverseLength;
    rotation.y *= inverseLength;
    rotation.z *= inverseLength;
    rotation.w *= inverseLength;
    return rotation;
}

Vec3 RotateVector(const Quaternion& sourceRotation, const Vec3& vector)
{
    const Quaternion q = NormalizeQuaternion(sourceRotation);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    return Vec3{
        (1.0f - 2.0f * (yy + zz)) * vector.x + 2.0f * (xy - wz) * vector.y +
            2.0f * (xz + wy) * vector.z,
        2.0f * (xy + wz) * vector.x + (1.0f - 2.0f * (xx + zz)) * vector.y +
            2.0f * (yz - wx) * vector.z,
        2.0f * (xz - wy) * vector.x + 2.0f * (yz + wx) * vector.y +
            (1.0f - 2.0f * (xx + yy)) * vector.z,
    };
}

Vec3 RotateHalfExtents(const Quaternion& sourceRotation, const Vec3& halfExtents)
{
    const Quaternion q = NormalizeQuaternion(sourceRotation);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    const float r00 = 1.0f - 2.0f * (yy + zz);
    const float r01 = 2.0f * (xy - wz);
    const float r02 = 2.0f * (xz + wy);
    const float r10 = 2.0f * (xy + wz);
    const float r11 = 1.0f - 2.0f * (xx + zz);
    const float r12 = 2.0f * (yz - wx);
    const float r20 = 2.0f * (xz - wy);
    const float r21 = 2.0f * (yz + wx);
    const float r22 = 1.0f - 2.0f * (xx + yy);
    return Vec3{
        std::fabs(r00) * halfExtents.x + std::fabs(r01) * halfExtents.y +
            std::fabs(r02) * halfExtents.z,
        std::fabs(r10) * halfExtents.x + std::fabs(r11) * halfExtents.y +
            std::fabs(r12) * halfExtents.z,
        std::fabs(r20) * halfExtents.x + std::fabs(r21) * halfExtents.y +
            std::fabs(r22) * halfExtents.z,
    };
}

Camera ParseCamera(const JsonValue& value)
{
    Camera camera{};
    camera.present = true;
    camera.isMainCamera = BoolOr(value, "isMainCamera", false);
    camera.fieldOfViewDegrees = FloatOr(value, "fov", camera.fieldOfViewDegrees);
    camera.nearPlane = FloatOr(value, "nearPlane", camera.nearPlane);
    camera.farPlane = FloatOr(value, "farPlane", camera.farPlane);
    camera.thirdPersonEnabled = BoolOr(value, "thirdPersonEnabled", false);
    camera.thirdPersonTargetName = StringOr(value, "thirdPersonTargetName");
    ReadVec3(Member(value, "thirdPersonTargetOffset"), camera.thirdPersonTargetOffset);
    camera.thirdPersonDistance = FloatOr(value, "thirdPersonDistance", camera.thirdPersonDistance);
    camera.thirdPersonYawDegrees = FloatOr(value, "thirdPersonYaw", camera.thirdPersonYawDegrees);
    camera.thirdPersonPitchDegrees = FloatOr(value, "thirdPersonPitch", camera.thirdPersonPitchDegrees);
    if (camera.fieldOfViewDegrees < 5.0f || camera.fieldOfViewDegrees > 175.0f) {
        camera.fieldOfViewDegrees = 60.0f;
    }
    if (camera.nearPlane <= 0.0f || camera.farPlane <= camera.nearPlane) {
        camera.nearPlane = 0.1f;
        camera.farPlane = 100.0f;
    }
    return camera;
}

Light ParseLight(const JsonValue& value, const Transform& transform)
{
    Light light{};
    light.type = IntOr(value, "type", light.type);
    light.transform = transform;
    ReadVec3(Member(value, "color"), light.color);
    light.intensity = FloatOr(value, "intensity", light.intensity);
    light.range = FloatOr(value, "range", light.range);
    light.spotAngleDegrees = FloatOr(value, "spotAngle", light.spotAngleDegrees);
    light.castShadow = BoolOr(value, "castShadow", light.castShadow);
    return light;
}

Skybox ParseSkybox(const JsonValue& value)
{
    Skybox skybox{};
    skybox.present = true;
    skybox.enabled = BoolOr(value, "enabled", true);
    skybox.textureName = StringOr(value, "textureName");
    ReadVec3(Member(value, "tint"), skybox.tint);
    skybox.intensity = FloatOr(value, "intensity", skybox.intensity);
    return skybox;
}

} // namespace

std::size_t Definition::StaticCubeCount() const
{
    return static_cast<std::size_t>(std::count_if(entities.begin(), entities.end(),
        [](const Entity& entity) { return entity.importAsStaticCube; }));
}

Transform RuntimeTransform(const Definition& definition, const Entity& entity,
    float runtimeGroundTopY)
{
    Transform transform = entity.transform;
    if (definition.hasGroundSurface) {
        transform.position.y += runtimeGroundTopY - definition.authoredGroundTopY;
    }
    return transform;
}

std::vector<StaticBoxCollider> BuildStaticBoxColliders(const Definition& definition,
    float runtimeGroundTopY)
{
    std::vector<StaticBoxCollider> colliders;
    colliders.reserve(definition.StaticCubeCount());
    for (const Entity& entity : definition.entities) {
        const ColliderSettings& settings = entity.collider;
        if (!entity.importAsStaticCube || !settings.present || settings.isTrigger ||
            settings.shapeType != 0) {
            continue;
        }

        const Transform transform = RuntimeTransform(definition, entity, runtimeGroundTopY);
        const bool followsModel = settings.syncWithModel || settings.autoFitToModel;
        const Vec3 colliderScale = followsModel
            ? Vec3{std::fabs(transform.scale.x), std::fabs(transform.scale.y),
                std::fabs(transform.scale.z)}
            : Vec3{1.0f, 1.0f, 1.0f};
        Vec3 halfExtents = settings.autoFitToModel
            ? Vec3{1.0f, 1.0f, 1.0f}
            : Vec3{std::fabs(settings.size.x) * 0.5f,
                std::fabs(settings.size.y) * 0.5f,
                std::fabs(settings.size.z) * 0.5f};
        halfExtents.x *= colliderScale.x;
        halfExtents.y *= colliderScale.y;
        halfExtents.z *= colliderScale.z;
        if (!std::isfinite(halfExtents.x) || !std::isfinite(halfExtents.y) ||
            !std::isfinite(halfExtents.z) || halfExtents.x <= 1.0e-4f ||
            halfExtents.y <= 1.0e-4f || halfExtents.z <= 1.0e-4f) {
            continue;
        }

        const Vec3 localOffset{
            settings.offset.x * colliderScale.x,
            settings.offset.y * colliderScale.y,
            settings.offset.z * colliderScale.z,
        };
        const Vec3 rotatedOffset = RotateVector(transform.rotation, localOffset);
        StaticBoxCollider collider{};
        collider.entityName = entity.name;
        collider.center = Vec3{
            transform.position.x + rotatedOffset.x,
            transform.position.y + rotatedOffset.y,
            transform.position.z + rotatedOffset.z,
        };
        if (settings.useOBB) {
            collider.halfExtents = halfExtents;
            collider.rotation = NormalizeQuaternion(transform.rotation);
        } else {
            // Mikan's non-OBB collider is axis aligned. Expand the scaled box
            // by the model rotation before dropping that rotation.
            collider.halfExtents = RotateHalfExtents(transform.rotation, halfExtents);
            collider.rotation = Quaternion{};
        }
        colliders.push_back(std::move(collider));
    }
    return colliders;
}

bool ParseMikanScene(const char* json, std::size_t size, Definition& output,
    std::string& error)
{
    error.clear();
    if (json == nullptr || size == 0 || size > kMaxJsonBytes) {
        error = "scene JSON is empty or exceeds the 8 MiB limit";
        return false;
    }
    JsonValue root;
    JsonParser parser(json, size);
    if (!parser.Parse(root, error)) {
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = "scene root must be a JSON object";
        return false;
    }

    Definition parsed{};
    parsed.formatVersion = IntOr(root, "formatVersion", 0);
    if (parsed.formatVersion != 1) {
        error = "only Mikan formatVersion 1 is supported";
        return false;
    }
    parsed.game = StringOr(root, "game");
    const JsonValue* entities = Member(root, "entities");
    if (entities == nullptr || entities->type != JsonValue::Type::Array ||
        entities->array.size() > kMaxEntities) {
        error = "scene entities must be an array with at most 4096 entries";
        return false;
    }

    bool hasCameraCandidate = false;
    std::size_t importedStaticCubes = 0;
    parsed.entities.reserve(entities->array.size());
    for (const JsonValue& source : entities->array) {
        if (source.type != JsonValue::Type::Object) {
            error = "every scene entity must be an object";
            return false;
        }
        Entity entity{};
        entity.id = UIntOr(Member(source, "id"), 0);
        entity.transform = ParseTransform(Member(source, "transform"));
        if (const JsonValue* name = Member(source, "name")) {
            entity.name = StringOr(*name, "name");
        }
        if (const JsonValue* hierarchy = Member(source, "hierarchy")) {
            entity.parentId = UIntOr(Member(*hierarchy, "parent"), kNoParent);
        }
        if (const JsonValue* mesh = Member(source, "mesh")) {
            entity.hasMesh = true;
            entity.modelPath = StringOr(*mesh, "modelPath");
            const std::string normalized = NormalizePath(entity.modelPath);
            if (normalized.find("base model/cube.glb") != std::string::npos ||
                normalized.find("basemodel/cube.glb") != std::string::npos) {
                entity.meshKind = MeshKind::UnitCube;
            } else {
                entity.meshKind = MeshKind::Unsupported;
            }
        }
        if (const JsonValue* render = Member(source, "render")) {
            entity.visible = BoolOr(*render, "visible", true);
            entity.castShadow = BoolOr(*render, "castShadow", true);
            entity.receiveShadow = BoolOr(*render, "receiveShadow", true);
        }
        const JsonValue* rigidBody = Member(source, "rigidBody");
        if (rigidBody != nullptr) {
            entity.hasRigidBody = true;
            entity.rigidBodyType = IntOr(*rigidBody, "type", -1);
            entity.collider = ParseColliderSettings(*rigidBody, true);
        }
        if (const JsonValue* collider = Member(source, "collider")) {
            entity.collider = ParseColliderSettings(*collider, false);
        }
        ParseMaterial(Member(source, "material"), entity.material);

        if (const JsonValue* cameraValue = Member(source, "camera")) {
            const Camera camera = ParseCamera(*cameraValue);
            if (camera.isMainCamera || !hasCameraCandidate) {
                parsed.mainCamera = camera;
                hasCameraCandidate = true;
            }
        }
        if (const JsonValue* lightValue = Member(source, "light")) {
            parsed.lights.push_back(ParseLight(*lightValue, entity.transform));
        }
        if (const JsonValue* skyboxValue = Member(source, "skybox")) {
            if (!parsed.skybox.present) {
                parsed.skybox = ParseSkybox(*skyboxValue);
            }
        }

        entity.groundSurface = IsGroundSurface(entity);
        if (entity.groundSurface && !parsed.hasGroundSurface) {
            parsed.hasGroundSurface = true;
            // The Mikan cube is [-1, 1] in model space and its local scale is
            // applied directly by both importers. This records the authored
            // floor top so each RHI can align it to the existing Jolt floor.
            parsed.authoredGroundTopY = entity.transform.position.y +
                std::fabs(entity.transform.scale.y);
        }
        const bool staticBody = !entity.hasRigidBody || entity.rigidBodyType == 0;
        const bool staticCubeCandidate = entity.hasMesh && entity.visible && staticBody &&
            entity.parentId == kNoParent && entity.meshKind == MeshKind::UnitCube &&
            !entity.groundSurface;
        entity.importAsStaticCube = staticCubeCandidate &&
            importedStaticCubes < kMaxStaticCubeRenderables;
        if (entity.importAsStaticCube) {
            if (!entity.collider.present) {
                // The Mikan cube model has local bounds [-1, 1]. OHOS has no
                // scene editor, so a serialized static cube without a
                // collider component receives a model-fitted solid box.
                entity.collider.present = true;
                entity.collider.shapeType = 0;
                entity.collider.size = Vec3{2.0f, 2.0f, 2.0f};
                entity.collider.syncWithModel = true;
                entity.collider.autoFitToModel = true;
            }
            ++importedStaticCubes;
        }
        if (entity.hasMesh && entity.visible && !entity.groundSurface &&
            !entity.importAsStaticCube) {
            ++parsed.unsupportedVisibleMeshCount;
        }
        parsed.entities.push_back(std::move(entity));
    }

    output = std::move(parsed);
    return true;
}

bool LoadMikanSceneRawFile(const char* rawFilePath, Definition& output,
    std::string& error)
{
    error.clear();
    if (rawFilePath == nullptr || rawFilePath[0] == '\0') {
        error = "rawfile path is empty";
        return false;
    }
    void* bytes = nullptr;
    std::size_t size = 0;
    if (!OHOS_ReadRawFile(rawFilePath, &bytes, &size) || bytes == nullptr || size == 0) {
        if (bytes != nullptr) {
            OHOS_FreeRawFile(bytes);
        }
        error = "rawfile could not be opened";
        return false;
    }
    const bool parsed = ParseMikanScene(static_cast<const char*>(bytes), size, output, error);
    OHOS_FreeRawFile(bytes);
    return parsed;
}

void BuildModelMatrix(const Transform& transform, float output[16])
{
    float x = transform.rotation.x;
    float y = transform.rotation.y;
    float z = transform.rotation.z;
    float w = transform.rotation.w;
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(length) || length < 1.0e-6f) {
        x = 0.0f;
        y = 0.0f;
        z = 0.0f;
        w = 1.0f;
    } else {
        const float inverseLength = 1.0f / length;
        x *= inverseLength;
        y *= inverseLength;
        z *= inverseLength;
        w *= inverseLength;
    }

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;
    output[0] = (1.0f - 2.0f * (yy + zz)) * transform.scale.x;
    output[1] = (2.0f * (xy + wz)) * transform.scale.x;
    output[2] = (2.0f * (xz - wy)) * transform.scale.x;
    output[3] = 0.0f;
    output[4] = (2.0f * (xy - wz)) * transform.scale.y;
    output[5] = (1.0f - 2.0f * (xx + zz)) * transform.scale.y;
    output[6] = (2.0f * (yz + wx)) * transform.scale.y;
    output[7] = 0.0f;
    output[8] = (2.0f * (xz + wy)) * transform.scale.z;
    output[9] = (2.0f * (yz - wx)) * transform.scale.z;
    output[10] = (1.0f - 2.0f * (xx + yy)) * transform.scale.z;
    output[11] = 0.0f;
    output[12] = transform.position.x;
    output[13] = transform.position.y;
    output[14] = transform.position.z;
    output[15] = 1.0f;
}

} // namespace scene
