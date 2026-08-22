#include "pch.h"

#include "ObjMesh.h"

#include "Log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace Neuron
{
namespace
{

using namespace DirectX;

constexpr const char* MATERIAL_NAMES[MESH_MATERIAL_COUNT] = {"hull", "plate", "glass", "accent", "thruster"};

/*
 * One whitespace-separated token, and where it started.
 *
 * The column travels with the token because a diagnostic that says only "line
 * 412" sends a person to a line with six numbers on it and no clue which one
 * the parser choked on.
 */
struct Token
{
  std::string_view text;
  std::uint32_t column = 1; // 1-based, as editors count.
};

[[nodiscard]] bool IsSpace(char _c) noexcept
{
  return _c == ' ' || _c == '\t' || _c == '\r' || _c == '\f' || _c == '\v';
}

/// Advances past whitespace and takes the next token. False at end of line.
[[nodiscard]] bool NextToken(std::string_view _line, std::size_t& _cursor, Token& _outToken) noexcept
{
  while (_cursor < _line.size() && IsSpace(_line[_cursor]))
  {
    ++_cursor;
  }
  if (_cursor >= _line.size())
  {
    return false;
  }

  const std::size_t start = _cursor;
  while (_cursor < _line.size() && !IsSpace(_line[_cursor]))
  {
    ++_cursor;
  }

  _outToken.text = _line.substr(start, _cursor - start);
  _outToken.column = static_cast<std::uint32_t>(start + 1);
  return true;
}

[[nodiscard]] bool ParseFloat(std::string_view _text, float& _outValue) noexcept
{
  // from_chars rather than strtof: no locale, so a machine whose decimal
  // separator is a comma still reads the same mesh as everyone else's.
  const char* first = _text.data();
  const char* last = first + _text.size();
  const std::from_chars_result result = std::from_chars(first, last, _outValue, std::chars_format::general);
  return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] bool ParseInt(std::string_view _text, std::int64_t& _outValue) noexcept
{
  const char* first = _text.data();
  const char* last = first + _text.size();
  const std::from_chars_result result = std::from_chars(first, last, _outValue);
  return result.ec == std::errc{} && result.ptr == last;
}

void Fail(ObjDiagnostic& _outError, std::uint32_t _line, std::uint32_t _column, std::string _message)
{
  _outError.line = _line;
  _outError.column = _column;
  _outError.message = std::move(_message);
}

/// The three slash-separated indices of one face corner. Zero means "absent",
/// which OBJ spells as an empty field (`7//3`) and which is legal.
struct FaceCorner
{
  std::int64_t position = 0;
  std::int64_t texture = 0;
  std::int64_t normal = 0;
};

[[nodiscard]] bool ParseFaceCorner(std::string_view _text, FaceCorner& _outCorner) noexcept
{
  std::int64_t* fields[3] = {&_outCorner.position, &_outCorner.texture, &_outCorner.normal};

  std::size_t fieldIndex = 0;
  std::size_t cursor = 0;
  while (true)
  {
    if (fieldIndex == 3)
    {
      return false; // A fourth field means this is not an OBJ corner at all.
    }

    const std::size_t slash = _text.find('/', cursor);
    const bool lastField = slash == std::string_view::npos;
    const std::string_view field = lastField ? _text.substr(cursor) : _text.substr(cursor, slash - cursor);

    if (!field.empty() && !ParseInt(field, *fields[fieldIndex]))
    {
      return false;
    }
    ++fieldIndex;

    if (lastField)
    {
      return true;
    }
    cursor = slash + 1;
  }
}

/// OBJ indices are 1-based, and negative means "counting back from the most
/// recent one" -- which is why resolution needs the count so far, not the total.
[[nodiscard]] bool ResolveIndex(std::int64_t _value, std::size_t _count, std::uint32_t& _outIndex) noexcept
{
  if (_value > 0 && static_cast<std::size_t>(_value) <= _count)
  {
    _outIndex = static_cast<std::uint32_t>(_value - 1);
    return true;
  }
  if (_value < 0 && static_cast<std::size_t>(-_value) <= _count)
  {
    _outIndex = static_cast<std::uint32_t>(static_cast<std::int64_t>(_count) + _value);
    return true;
  }
  return false;
}

XMFLOAT3 FaceNormal(const XMFLOAT3& _a, const XMFLOAT3& _b, const XMFLOAT3& _c) noexcept
{
  const XMVECTOR edge0 = XMVectorSubtract(XMLoadFloat3(&_b), XMLoadFloat3(&_a));
  const XMVECTOR edge1 = XMVectorSubtract(XMLoadFloat3(&_c), XMLoadFloat3(&_a));
  XMFLOAT3 normal;
  XMStoreFloat3(&normal, XMVector3Normalize(XMVector3Cross(edge0, edge1)));
  return normal;
}

/*
 * Builds the vertex buffer as faces are read.
 *
 * Two corners that name the same position *and* the same normal are the same
 * vertex; two that share a position across a hard edge are not. Keying on the
 * pair is what keeps flat shading flat without the shader knowing about it.
 */
class VertexBuilder
{
public:
  explicit VertexBuilder(std::vector<MeshVertex>& _vertices) noexcept : m_vertices(_vertices) {}

  [[nodiscard]] std::uint32_t Add(const XMFLOAT3& _position, std::uint32_t _positionIndex, const XMFLOAT3& _normal,
                                  std::uint32_t _normalIndex)
  {
    const std::uint64_t key = (static_cast<std::uint64_t>(_positionIndex) << 32) | _normalIndex;
    const auto existing = m_lookup.find(key);
    if (existing != m_lookup.end())
    {
      return existing->second;
    }

    const auto index = static_cast<std::uint32_t>(m_vertices.size());
    m_vertices.push_back(MeshVertex{_position, _normal});
    m_lookup.emplace(key, index);
    return index;
  }

  /// For corners with no authored normal: the face normal is computed, so two
  /// corners at the same position genuinely differ and must not be merged.
  [[nodiscard]] std::uint32_t AddUnshared(const XMFLOAT3& _position, const XMFLOAT3& _normal)
  {
    const auto index = static_cast<std::uint32_t>(m_vertices.size());
    m_vertices.push_back(MeshVertex{_position, _normal});
    return index;
  }

private:
  std::vector<MeshVertex>& m_vertices;
  std::unordered_map<std::uint64_t, std::uint32_t> m_lookup;
};

void ComputeBounds(ObjMesh& _mesh) noexcept
{
  if (_mesh.vertices.empty())
  {
    return;
  }

  XMVECTOR minimum = XMLoadFloat3(&_mesh.vertices[0].position);
  XMVECTOR maximum = minimum;
  float radiusSquared = 0.0f;

  for (const MeshVertex& vertex : _mesh.vertices)
  {
    const XMVECTOR position = XMLoadFloat3(&vertex.position);
    minimum = XMVectorMin(minimum, position);
    maximum = XMVectorMax(maximum, position);
    radiusSquared = std::max(radiusSquared, XMVectorGetX(XMVector3LengthSq(position)));
  }

  XMStoreFloat3(&_mesh.boundsMin, minimum);
  XMStoreFloat3(&_mesh.boundsMax, maximum);
  _mesh.radiusMetres = std::sqrt(radiusSquared);
}

/// Win32 rather than <fstream>: the rest of the tree reads files this way, and
/// a missing file has to come back as a diagnostic rather than an exception.
[[nodiscard]] bool ReadWholeFile(const std::string& _path, std::string& _outText)
{
  const int wide = MultiByteToWideChar(CP_UTF8, 0, _path.c_str(), -1, nullptr, 0);
  std::wstring widePath(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _path.c_str(), -1, widePath.data(), wide);

  const HANDLE file = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    return false;
  }

  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == 0 || size.QuadPart < 0 || size.QuadPart > 0x7fffffff)
  {
    CloseHandle(file);
    return false;
  }

  _outText.resize(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = _outText.empty() ? TRUE : ReadFile(file, _outText.data(), static_cast<DWORD>(_outText.size()), &read, nullptr);
  CloseHandle(file);

  if (ok == 0 || read != _outText.size())
  {
    return false;
  }
  return true;
}

std::string JoinPath(std::string_view _directory, std::string_view _fileName)
{
  std::string path(_directory);
  if (!path.empty() && path.back() != '/' && path.back() != '\\')
  {
    path += '/';
  }
  path.append(_fileName);
  return path;
}

} // namespace

bool ParseMeshMaterialName(std::string_view _name, MeshMaterial& _outMaterial) noexcept
{
  for (std::uint32_t i = 0; i < MESH_MATERIAL_COUNT; ++i)
  {
    if (_name == MATERIAL_NAMES[i])
    {
      _outMaterial = static_cast<MeshMaterial>(i);
      return true;
    }
  }
  return false;
}

const char* MeshMaterialName(MeshMaterial _material) noexcept
{
  const auto index = static_cast<std::uint32_t>(_material);
  return index < MESH_MATERIAL_COUNT ? MATERIAL_NAMES[index] : "?";
}

std::string ObjDiagnostic::Text() const
{
  std::string text = file;
  text += '(';
  text += std::to_string(line);
  text += ',';
  text += std::to_string(column);
  text += "): ";
  text += message;
  return text;
}

bool ParseObjMesh(std::string_view _text, ObjMesh& _outMesh, ObjDiagnostic& _outError)
{
  _outMesh = ObjMesh{};

  std::vector<XMFLOAT3> positions;
  std::vector<XMFLOAT3> normals;
  std::size_t textureCount = 0;

  // Triangles accumulate per material and are concatenated at the end, which is
  // what turns 117 interleaved `usemtl` groups in Structure.obj into five draws.
  std::vector<std::uint32_t> perMaterial[MESH_MATERIAL_COUNT];

  VertexBuilder builder(_outMesh.vertices);

  bool materialSelected = false;
  auto material = MeshMaterial::Hull;

  std::vector<FaceCorner> corners;
  std::uint32_t lineNumber = 0;
  std::size_t lineStart = 0;

  while (lineStart <= _text.size())
  {
    const std::size_t lineEnd = std::min(_text.find('\n', lineStart), _text.size());
    const std::string_view line = _text.substr(lineStart, lineEnd - lineStart);
    ++lineNumber;
    lineStart = lineEnd + 1;

    std::size_t cursor = 0;
    Token keyword;
    if (!NextToken(line, cursor, keyword) || keyword.text[0] == '#')
    {
      if (lineEnd == _text.size())
      {
        break;
      }
      continue;
    }

    if (keyword.text == "v" || keyword.text == "vn")
    {
      float value[3] = {0.0f, 0.0f, 0.0f};
      for (float& component : value)
      {
        Token token;
        if (!NextToken(line, cursor, token))
        {
          Fail(_outError, lineNumber, keyword.column, std::string(keyword.text) + " needs three numbers");
          return false;
        }
        if (!ParseFloat(token.text, component))
        {
          Fail(_outError, lineNumber, token.column, "'" + std::string(token.text) + "' is not a number");
          return false;
        }
      }

      // A `v` may carry a fourth weight component and a colour; neither means
      // anything to a flat-shaded hull, so they are read past rather than
      // rejected.
      (keyword.text == "v" ? positions : normals).push_back(XMFLOAT3{value[0], value[1], value[2]});
    }
    else if (keyword.text == "vt")
    {
      ++textureCount; // Counted only so a `v/vt/vn` corner can be range-checked.
    }
    else if (keyword.text == "usemtl")
    {
      Token token;
      if (!NextToken(line, cursor, token))
      {
        Fail(_outError, lineNumber, keyword.column, "usemtl needs a material name");
        return false;
      }
      if (!ParseMeshMaterialName(token.text, material))
      {
        Fail(_outError, lineNumber, token.column,
             "unknown material '" + std::string(token.text) + "' (expected hull, plate, glass, accent or thruster)");
        return false;
      }
      materialSelected = true;
    }
    else if (keyword.text == "mtllib")
    {
      Token token;
      if (!NextToken(line, cursor, token))
      {
        Fail(_outError, lineNumber, keyword.column, "mtllib needs a file name");
        return false;
      }
      _outMesh.materialLibrary.assign(token.text);
    }
    else if (keyword.text == "f")
    {
      if (!materialSelected)
      {
        Fail(_outError, lineNumber, keyword.column, "face before any usemtl -- every face belongs to one of the five materials");
        return false;
      }

      corners.clear();
      Token token;
      while (NextToken(line, cursor, token))
      {
        FaceCorner corner;
        if (!ParseFaceCorner(token.text, corner))
        {
          Fail(_outError, lineNumber, token.column, "'" + std::string(token.text) + "' is not a v/vt/vn corner");
          return false;
        }
        if (corner.position == 0)
        {
          Fail(_outError, lineNumber, token.column, "a face corner needs a position index");
          return false;
        }
        corners.push_back(corner);
      }

      if (corners.size() < 3)
      {
        Fail(_outError, lineNumber, keyword.column, "a face needs at least three corners");
        return false;
      }

      // Fan triangulation. The corpus is triangulated already, so this is the
      // path a hand-edit takes, not the path the shipped meshes take.
      std::vector<std::uint32_t>& bucket = perMaterial[static_cast<std::uint32_t>(material)];
      for (std::size_t apex = 1; apex + 1 < corners.size(); ++apex)
      {
        const FaceCorner* triangle[3] = {&corners[0], &corners[apex], &corners[apex + 1]};

        std::uint32_t positionIndex[3] = {};
        std::uint32_t normalIndex[3] = {};
        bool haveNormals = true;

        for (std::size_t i = 0; i < 3; ++i)
        {
          if (!ResolveIndex(triangle[i]->position, positions.size(), positionIndex[i]))
          {
            Fail(_outError, lineNumber, keyword.column, "vertex index " + std::to_string(triangle[i]->position) + " is out of range");
            return false;
          }
          if (triangle[i]->texture != 0)
          {
            std::uint32_t unused = 0;
            if (!ResolveIndex(triangle[i]->texture, textureCount, unused))
            {
              Fail(_outError, lineNumber, keyword.column, "texture index " + std::to_string(triangle[i]->texture) + " is out of range");
              return false;
            }
          }
          if (triangle[i]->normal != 0)
          {
            if (!ResolveIndex(triangle[i]->normal, normals.size(), normalIndex[i]))
            {
              Fail(_outError, lineNumber, keyword.column, "normal index " + std::to_string(triangle[i]->normal) + " is out of range");
              return false;
            }
          }
          else
          {
            haveNormals = false;
          }
        }

        if (haveNormals)
        {
          for (std::size_t i = 0; i < 3; ++i)
          {
            bucket.push_back(
                builder.Add(positions[positionIndex[i]], positionIndex[i], normals[normalIndex[i]], normalIndex[i]));
          }
        }
        else
        {
          // No authored normal: derive one for the triangle. Flat is the target
          // shading model anyway, so this is a substitution, not a compromise.
          const XMFLOAT3 normal = FaceNormal(positions[positionIndex[0]], positions[positionIndex[1]], positions[positionIndex[2]]);
          for (std::size_t i = 0; i < 3; ++i)
          {
            bucket.push_back(builder.AddUnshared(positions[positionIndex[i]], normal));
          }
        }
      }
    }

    if (lineEnd == _text.size())
    {
      break;
    }
  }

  std::size_t total = 0;
  for (const std::vector<std::uint32_t>& bucket : perMaterial)
  {
    total += bucket.size();
  }
  if (total == 0)
  {
    Fail(_outError, lineNumber, 1, "no faces -- this is not a mesh");
    return false;
  }

  _outMesh.indices.reserve(total);
  for (std::uint32_t i = 0; i < MESH_MATERIAL_COUNT; ++i)
  {
    const std::vector<std::uint32_t>& bucket = perMaterial[i];
    if (bucket.empty())
    {
      continue; // A mesh with no glass gets no glass submesh, not an empty draw.
    }

    SubmeshRange range;
    range.firstIndex = static_cast<std::uint32_t>(_outMesh.indices.size());
    range.indexCount = static_cast<std::uint32_t>(bucket.size());
    range.material = static_cast<MeshMaterial>(i);
    _outMesh.submeshes.push_back(range);

    _outMesh.indices.insert(_outMesh.indices.end(), bucket.begin(), bucket.end());
  }

  ComputeBounds(_outMesh);
  return true;
}

bool ParseMeshMaterials(std::string_view _text, MeshMaterialPalette& _outPalette, ObjDiagnostic& _outError)
{
  _outPalette = MeshMaterialPalette{};

  bool haveMaterial = false;
  auto material = MeshMaterial::Hull;

  std::uint32_t lineNumber = 0;
  std::size_t lineStart = 0;

  while (lineStart <= _text.size())
  {
    const std::size_t lineEnd = std::min(_text.find('\n', lineStart), _text.size());
    const std::string_view line = _text.substr(lineStart, lineEnd - lineStart);
    ++lineNumber;
    lineStart = lineEnd + 1;

    std::size_t cursor = 0;
    Token keyword;
    if (NextToken(line, cursor, keyword) && keyword.text[0] != '#')
    {
      if (keyword.text == "newmtl")
      {
        Token token;
        if (!NextToken(line, cursor, token))
        {
          Fail(_outError, lineNumber, keyword.column, "newmtl needs a material name");
          return false;
        }
        if (!ParseMeshMaterialName(token.text, material))
        {
          Fail(_outError, lineNumber, token.column,
               "unknown material '" + std::string(token.text) + "' (expected hull, plate, glass, accent or thruster)");
          return false;
        }
        haveMaterial = true;
      }
      else if (keyword.text == "Kd")
      {
        if (!haveMaterial)
        {
          Fail(_outError, lineNumber, keyword.column, "Kd before any newmtl");
          return false;
        }

        float colour[3] = {0.0f, 0.0f, 0.0f};
        for (float& component : colour)
        {
          Token token;
          if (!NextToken(line, cursor, token))
          {
            Fail(_outError, lineNumber, keyword.column, "Kd needs three numbers");
            return false;
          }
          if (!ParseFloat(token.text, component))
          {
            Fail(_outError, lineNumber, token.column, "'" + std::string(token.text) + "' is not a number");
            return false;
          }
        }

        const auto index = static_cast<std::uint32_t>(material);
        _outPalette.albedo[index] = XMFLOAT3{colour[0], colour[1], colour[2]};
        _outPalette.defined[index] = true;
      }
      // Ks, Ns and d are read past: the MVP shading model is albedo plus a
      // renderer-side emissive strength, so there is nothing to store them in.
    }

    if (lineEnd == _text.size())
    {
      break;
    }
  }

  return true;
}

float PlaneRadiusMetres(const ObjMesh& _mesh) noexcept
{
  float radiusSquared = 0.0f;
  for (const MeshVertex& vertex : _mesh.vertices)
  {
    const float x = vertex.position.x;
    const float z = vertex.position.z;
    radiusSquared = std::max(radiusSquared, x * x + z * z);
  }
  return std::sqrt(radiusSquared);
}

float LocalTopMetres(const ObjMesh& _mesh, float _x, float _z, float _halfExtentX, float _halfExtentZ, float _fallback) noexcept
{
  float top = -std::numeric_limits<float>::max();
  for (const MeshVertex& vertex : _mesh.vertices)
  {
    if (std::fabs(vertex.position.x - _x) <= _halfExtentX && std::fabs(vertex.position.z - _z) <= _halfExtentZ)
    {
      top = std::max(top, vertex.position.y);
    }
  }
  return top > -std::numeric_limits<float>::max() ? top : _fallback;
}

bool FitObjMeshToPlaneRadius(ObjMesh& _mesh, float _targetPlaneRadiusMetres) noexcept
{
  if (_targetPlaneRadiusMetres <= 0.0f)
  {
    return false; // The caller has no opinion. Leave the art alone.
  }

  const float authored = PlaneRadiusMetres(_mesh);
  if (authored <= 0.0f)
  {
    return false; // Nothing on the plane to measure -- a mesh with no width
                  // cannot be given one, and dividing by it would be worse.
  }

  const float factor = _targetPlaneRadiusMetres / authored;
  for (MeshVertex& vertex : _mesh.vertices)
  {
    // Positions only. A *uniform* scale leaves a unit normal unit, so touching
    // them would cost a normalise per vertex to arrive back where they started
    // -- and would quietly stop being true the day somebody wanted a
    // non-uniform one, which is the version worth failing loudly.
    vertex.position.x *= factor;
    vertex.position.y *= factor;
    vertex.position.z *= factor;
  }

  // Recomputed rather than multiplied through: one function decides how big a
  // mesh is, and it is the one that decided before.
  ComputeBounds(_mesh);
  return true;
}

bool LoadObjMesh(std::string_view _directory, std::string_view _fileName, ObjMesh& _outMesh, ObjDiagnostic& _outError)
{
  const std::string meshPath = JoinPath(_directory, _fileName);
  _outError = ObjDiagnostic{};
  _outError.file.assign(_fileName);

  std::string text;
  if (!ReadWholeFile(meshPath, text))
  {
    Fail(_outError, 0, 0, "could not read " + meshPath);
    return false;
  }

  if (!ParseObjMesh(text, _outMesh, _outError))
  {
    return false;
  }

  if (_outMesh.materialLibrary.empty())
  {
    // No mtllib is not fatal: the five canonical colours have renderer-side
    // defaults, and a mesh that names no library simply takes them.
    NEURON_LOG_WARNING("%s names no mtllib; using the default palette", meshPath.c_str());
    return true;
  }

  const std::string materialPath = JoinPath(_directory, _outMesh.materialLibrary);
  std::string materialText;
  if (!ReadWholeFile(materialPath, materialText))
  {
    Fail(_outError, 0, 0, "could not read " + materialPath + ", named by " + meshPath);
    return false;
  }

  ObjDiagnostic materialError;
  materialError.file = _outMesh.materialLibrary;
  if (!ParseMeshMaterials(materialText, _outMesh.palette, materialError))
  {
    _outError = materialError;
    return false;
  }

  return true;
}

} // namespace Neuron
