// 던전 목록 파일(dungeons.json)의 모양과 읽기. 사람이 고치는 입력이다(결정 170 U-1).
// 필수 필드가 빠지거나 모르는 필드가 있으면 읽지 않는다(Json.Strict).
using System.Text.Json;
using System.Text.Json.Serialization;

namespace SkyrimPoE.Tools.DungeonClone;

internal sealed class PackSpec
{
    // 결정 169, R4: 런타임의 MAPS v2 키 길이와 같다. 키 문자는 아래에서 ASCII로 제한한다.
    private const int MaxDungeonKeyBytes = 64;
    public const string RuntimePluginName = "SkyrimPoE_Maps.esp";
    // 지도 팩 플러그인 파일 이름(결정 20: 정규 ESP 하나)
    [JsonRequired] public string Plugin { get; set; } = "";
    // 런타임이 세이브의 지도와 팩의 짝을 확인하는 판 번호. 던전을 더하거나 규칙을 바꾸면 올린다.
    [JsonRequired] public int PackVersion { get; set; }
    [JsonRequired] public string EditorIdPrefix { get; set; } = "";
    // 입구에서 이 거리(유닛, 3차원 직선) 안의 앵커는 쓰지 않는다(6단계 계획 R1 3번).
    [JsonRequired] public float AnchorMinDistanceFromEntry { get; set; }
    // 앵커를 내비메쉬로 옮길 수 있는 최대 거리. 넘으면 무리 앵커는 버리고, 보스 앵커면 실패한다.
    [JsonRequired] public float AnchorMaxSnapDistance { get; set; }
    // 앵커끼리 이보다 가까우면 뒤의 것을 버린다(같은 자리에 무리가 겹치지 않게).
    [JsonRequired] public float AnchorMinSeparation { get; set; }
    [JsonRequired] public List<DungeonSpec> Dungeons { get; set; } = new();

    public static PackSpec Load(string path)
    {
        var spec = JsonSerializer.Deserialize<PackSpec>(File.ReadAllText(path), Json.Strict)
                   ?? throw new InvalidDataException($"던전 목록을 읽지 못했습니다: {path}");
        spec.Validate();
        return spec;
    }

    private void Validate()
    {
        // 런타임(plugin/src/core/MapPack.h kMapsPluginName)은 이 이름만 받는다(결정 20: 정규 ESP).
        if (!string.Equals(Plugin, RuntimePluginName, StringComparison.Ordinal))
            throw new InvalidDataException($"plugin은 {RuntimePluginName}이어야 합니다(런타임이 이 이름만 읽음).");
        if (PackVersion <= 0) throw new InvalidDataException("packVersion은 1 이상이어야 합니다.");
        if (string.IsNullOrWhiteSpace(EditorIdPrefix)) throw new InvalidDataException("editorIdPrefix가 비었습니다.");
        if (AnchorMinDistanceFromEntry <= 0) throw new InvalidDataException("anchorMinDistanceFromEntry는 0보다 커야 합니다.");
        if (AnchorMaxSnapDistance <= 0) throw new InvalidDataException("anchorMaxSnapDistance는 0보다 커야 합니다.");
        if (AnchorMinSeparation <= 0) throw new InvalidDataException("anchorMinSeparation은 0보다 커야 합니다.");
        if (Dungeons.Count == 0) throw new InvalidDataException("dungeons가 비었습니다.");
        var keys = new HashSet<string>(StringComparer.Ordinal);
        var cells = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var d in Dungeons)
        {
            if (string.IsNullOrWhiteSpace(d.Key) || d.Key.Length > MaxDungeonKeyBytes ||
                d.Key.Any(ch => !(char.IsAsciiLetterLower(ch) || char.IsAsciiDigit(ch) || ch == '_')))
                throw new InvalidDataException($"key는 소문자·숫자·밑줄로 {MaxDungeonKeyBytes}자까지 씁니다: '{d.Key}'");
            if (!keys.Add(d.Key)) throw new InvalidDataException($"key가 겹칩니다: {d.Key}");
            if (!cells.Add(d.SourceCell)) throw new InvalidDataException($"sourceCell이 겹칩니다: {d.SourceCell}");
            if (string.IsNullOrWhiteSpace(d.Name)) throw new InvalidDataException($"{d.Key}: name이 비었습니다.");
            if (string.IsNullOrWhiteSpace(d.Type)) throw new InvalidDataException($"{d.Key}: type이 비었습니다.");
            if (d.Role is not ("map" or "arena")) throw new InvalidDataException($"{d.Key}: role은 map 또는 arena입니다.");
            if (d.MinPackAnchors < 1) throw new InvalidDataException($"{d.Key}: minPackAnchors는 1 이상이어야 합니다.");
            if (d.BossAnchor is { } b && !(b.Length == 6 && b.All(Uri.IsHexDigit))) throw new InvalidDataException($"{d.Key}: bossAnchor는 6자리 16진수 원본 참조 번호입니다.");
            if (d.BossAnchor is not null && string.IsNullOrWhiteSpace(d.BossAnchorReason)) throw new InvalidDataException($"{d.Key}: bossAnchor를 정했으면 bossAnchorReason도 적어야 합니다.");
        }
        // 런타임 목록 파서와 같은 조건: 지도 하나 이상, 결투장 정확히 하나(결정 18)
        if (Dungeons.Count(d => d.Role == "arena") != 1) throw new InvalidDataException("결투장(role=arena)은 정확히 하나 둡니다(결정 18).");
        if (!Dungeons.Any(d => d.Role == "map")) throw new InvalidDataException("지도(role=map)가 하나 이상 있어야 합니다.");
    }
}

internal sealed class DungeonSpec
{
    // 세이브와 런타임이 쓰는 바뀌지 않는 문자열 키(6단계 계획 R4 1번)
    [JsonRequired] public string Key { get; set; } = "";
    [JsonRequired] public string Type { get; set; } = "";
    [JsonRequired] public string Role { get; set; } = "";
    [JsonRequired] public string Name { get; set; } = "";
    // Skyrim.esm의 실내 셀 EditorID
    [JsonRequired] public string SourceCell { get; set; } = "";
    // 보스 앵커로 쓸 원본 NPC 참조(Skyrim.esm 로컬 번호, 6자리 16진수). Boss 표시가 하나뿐이면 비워도 된다.
    public string? BossAnchor { get; set; }
    // 왜 그 보스 앵커를 골랐는지(사람이 읽는 기록)
    public string? BossAnchorReason { get; set; }
    [JsonRequired] public int MinPackAnchors { get; set; }
    // 결정 173: 원형은 Skyrim.esm의 NPC_ 로컬 번호다.
    [JsonRequired] public string BossNpc { get; set; } = "";
    public List<string>? PackNpcs { get; set; }
    public List<string>? AddNpcs { get; set; }
}

internal static class Json
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    // 읽기용: 모르는 필드를 거부한다.
    public static readonly JsonSerializerOptions Strict = new(Options)
    {
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    };
}
