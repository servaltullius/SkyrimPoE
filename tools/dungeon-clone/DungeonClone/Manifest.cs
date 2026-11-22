// 런타임 목록 파일(지도 팩 ESP 옆의 .json). DLL은 손으로 옮긴 번호 대신 이것을 읽는다(6단계 계획 R1 7번, R4 1~3번).
// 복제 레코드는 지도 팩의 로컬 FormID, 원본 셀과 스폰 원형은 Skyrim.esm의 로컬 FormID다.
using System.Text.Json.Serialization;

namespace SkyrimPoE.Tools.DungeonClone;

internal sealed class PackManifest
{
    public const int CurrentFormat = 3;

    [JsonRequired] public int ManifestFormat { get; set; } = CurrentFormat;
    [JsonRequired] public string Plugin { get; set; } = "";
    [JsonRequired] public int PackVersion { get; set; }
    // 함께 설치된 ESP가 이 목록과 짝인지 런타임이 확인한다(SHA-256, 소문자 16진수).
    [JsonRequired] public string EspSha256 { get; set; } = "";
    [JsonRequired] public string SourceMaster { get; set; } = "Skyrim.esm";
    [JsonRequired] public List<DungeonManifest> Dungeons { get; set; } = new();
}

internal sealed class DungeonManifest
{
    [JsonRequired] public string Key { get; set; } = "";
    [JsonRequired] public string Type { get; set; } = "";
    [JsonRequired] public string Role { get; set; } = "";
    [JsonRequired] public string Name { get; set; } = "";
    [JsonRequired] public string SourceCell { get; set; } = "";
    [JsonRequired] public string Cell { get; set; } = "";
    [JsonRequired] public float NavmeshMinZ { get; set; }
    [JsonRequired] public string EntryMarker { get; set; } = "";
    [JsonRequired] public string BossAnchor { get; set; } = "";
    [JsonRequired] public List<string> PackAnchors { get; set; } = new();
    [JsonRequired] public string BossNpc { get; set; } = "";
    public List<string>? PackNpcs { get; set; }
    public List<string>? AddNpcs { get; set; }
    // 직접 여는 바깥문(순간이동을 지움). 런타임은 이 문을 열면 귀환을 묻는다.
    [JsonRequired] public List<string> ReturnDoors { get; set; } = new();
    // 닿으면 넘어가던 자동 문(순간이동을 지움). 그 너머는 빈 공간일 수 있다(실험 G-7).
    [JsonRequired] public List<AutoExit> AutoExits { get; set; } = new();
    // 다른 실내 칸으로 가던, 직접 여는 문(순간이동을 지움). 열어도 아무 데도 가지 않는다(물리적으로 막은 것은 아님).
    [JsonRequired] public List<string> SealedDoors { get; set; } = new();
}

internal sealed class AutoExit
{
    [JsonRequired] public string Id { get; set; } = "";
    // 원래 순간이동 목적지: exterior(야외) 또는 interior(다른 실내 칸)
    [JsonRequired] public string Leads { get; set; } = "";
    [JsonRequired] public float[] Position { get; set; } = Array.Empty<float>();
    [JsonRequired] public float[] Rotation { get; set; } = Array.Empty<float>();
}
