// FormID 장부(formid-ledger.json). 판이 바뀌어도 같은 원본·역할에는 같은 출력 번호를 준다(6단계 계획 R1 2번).
// 규칙:
// - 던전마다 겹치지 않는 번호 구간(slot)을 하나 준다. 구간은 처음 등장한 순서로 뒤에 붙고, 목록 순서를 바꿔도 그대로다.
// - 구간은 원본 셀(예: Skyrim.esm:0B6BE6)에 묶인다. 같은 키에 다른 원본 셀을 주면 갱신을 허락해도 실패한다.
// - 키는 "원본 플러그인:원본 로컬 번호:역할"이다(역할: cell, ref, anchor).
// - 구간 안의 번호는 next부터 하나씩 늘려 준다. 한 번 준 번호는 지우지 않으므로 [base, next)의 모든 번호가 정확히 한 번씩 쓰인다(밀도 검사).
//   빠진 참조의 번호가 다른 참조에 다시 쓰이지 않는다.
// - 장부를 옛 사본으로 되돌리거나 값을 맞바꾼 것은 장부 하나만 봐서는 알 수 없다. 커밋된 이전 장부와 diff 명령으로 비교한다.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace SkyrimPoE.Tools.DungeonClone;

internal sealed class Ledger
{
    public const uint FirstBase = 0x000800;
    public const uint SlotSize = 0x2000;
    public const uint MaxLocalId = 0xFFFFFF;

    [JsonRequired] public int Version { get; set; } = 2;
    [JsonRequired] public List<LedgerSlot> Slots { get; set; } = new();

    [JsonIgnore]
    public int NewAssignments { get; private set; }

    public static Ledger Load(string path, bool createIfMissing)
    {
        if (!File.Exists(path))
        {
            if (createIfMissing) return new Ledger();
            throw new FileNotFoundException($"장부가 없습니다: {path}. 처음 만들 때만 --create-ledger를 줍니다.");
        }
        var ledger = JsonSerializer.Deserialize<Ledger>(File.ReadAllText(path), Json.Strict)
                     ?? throw new InvalidDataException($"장부를 읽지 못했습니다: {path}");
        ledger.Validate();
        return ledger;
    }

    // 임시 파일에 쓴 뒤 바꿔 넣는다(쓰는 도중 실패해도 기존 장부가 남음).
    public void Save(string path)
    {
        Validate();
        foreach (var s in Slots)
        {
            s.Ids = new SortedDictionary<string, string>(s.Ids, StringComparer.Ordinal);
        }
        var text = JsonSerializer.Serialize(this, Json.Options).Replace("\r\n", "\n") + "\n";
        AtomicFile.WriteText(path, text);
    }

    public void Validate()
    {
        if (Version != 2) throw new InvalidDataException($"지원하지 않는 장부 판: {Version}");
        var seenDungeons = new HashSet<string>(StringComparer.Ordinal);
        var seenSources = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        uint expectedBase = FirstBase;
        foreach (var s in Slots)
        {
            if (!seenDungeons.Add(s.Dungeon)) throw new InvalidDataException($"장부: 던전 구간이 겹칩니다: {s.Dungeon}");
            if (!seenSources.Add(s.Source)) throw new InvalidDataException($"장부: 원본 셀 {s.Source}가 두 구간에 묶였습니다.");
            if (!IsSourceKey(s.Source)) throw new InvalidDataException($"장부: {s.Dungeon}의 source 형식이 틀렸습니다: {s.Source}");
            var b = ParseId(s.Base);
            if (b != expectedBase) throw new InvalidDataException($"장부: {s.Dungeon}의 구간 시작이 0x{b:X6}인데 0x{expectedBase:X6}이어야 합니다(구간은 순서대로 붙음).");
            if (s.Size != SlotSize) throw new InvalidDataException($"장부: {s.Dungeon}의 구간 크기가 0x{SlotSize:X}가 아닙니다.");
            var next = ParseId(s.Next);
            if (next < b || next > b + s.Size) throw new InvalidDataException($"장부: {s.Dungeon}의 next가 구간 밖입니다.");
            var used = new HashSet<uint>();
            foreach (var (k, v) in s.Ids)
            {
                if (!IsEntryKey(k)) throw new InvalidDataException($"장부: {s.Dungeon}의 키 형식이 틀렸습니다: {k}");
                var id = ParseId(v);
                if (id < b || id >= next) throw new InvalidDataException($"장부: {s.Dungeon}의 {k}=0x{id:X6}이 준 범위(0x{b:X6}..next) 밖입니다.");
                if (!used.Add(id)) throw new InvalidDataException($"장부: 번호 0x{id:X6}이 두 번 쓰였습니다.");
            }
            // 밀도: 준 번호는 지우지 않으므로 [base, next)가 빈틈없이 쓰여 있어야 한다.
            if (used.Count != next - b) throw new InvalidDataException($"장부: {s.Dungeon}의 [base, next)에 빈 번호가 {next - b - used.Count}개 있습니다(지운 키가 있음).");
            expectedBase = b + s.Size;
            if (expectedBase - 1 > MaxLocalId) throw new InvalidDataException("장부: 번호 공간을 넘었습니다.");
        }
    }

    public LedgerSlot GetSlot(string dungeon, string source, bool allowNew)
    {
        var slot = Slots.FirstOrDefault(s => s.Dungeon == dungeon);
        if (slot is not null)
        {
            if (!string.Equals(slot.Source, source, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"장부: 던전 '{dungeon}'은 원본 {slot.Source}에 묶였는데 목록은 {source}입니다. 원본을 바꾸려면 새 키를 쓰세요(세이브가 가진 번호가 다른 던전을 가리키게 됨).");
            return slot;
        }
        var other = Slots.FirstOrDefault(s => string.Equals(s.Source, source, StringComparison.OrdinalIgnoreCase));
        if (other is not null)
            throw new InvalidDataException($"장부: 원본 {source}는 이미 던전 '{other.Dungeon}'에 묶였습니다. 키를 바꾸면 번호가 모두 바뀝니다.");
        if (!allowNew) throw new LedgerChangeRequiredException($"장부에 던전 '{dungeon}'의 구간이 없습니다. --update-ledger로 만들어야 합니다.");
        var b = Slots.Count == 0 ? FirstBase : ParseId(Slots[^1].Base) + SlotSize;
        if (b + SlotSize - 1 > MaxLocalId) throw new InvalidDataException("장부: 번호 공간을 넘었습니다.");
        slot = new LedgerSlot { Dungeon = dungeon, Source = source, Base = Format(b), Size = SlotSize, Next = Format(b) };
        Slots.Add(slot);
        return slot;
    }

    // 원본·역할 키에 번호를 준다. 이미 있으면 같은 번호.
    public uint Assign(LedgerSlot slot, string key, bool allowNew)
    {
        if (!IsEntryKey(key)) throw new ArgumentException($"장부 키 형식이 틀렸습니다: {key}");
        if (slot.Ids.TryGetValue(key, out var existing)) return ParseId(existing);
        if (!allowNew) throw new LedgerChangeRequiredException($"장부의 '{slot.Dungeon}'에 '{key}'의 번호가 없습니다. --update-ledger로 만들어야 합니다.");
        var b = ParseId(slot.Base);
        var next = ParseId(slot.Next);
        if (next >= b + slot.Size) throw new InvalidDataException($"장부: '{slot.Dungeon}'의 구간(0x{slot.Size:X}개)을 다 썼습니다.");
        slot.Ids[key] = Format(next);
        slot.Next = Format(next + 1);
        NewAssignments++;
        return next;
    }

    // 이전 장부와 비교: 이전에 있던 것은 모두 그대로여야 한다(새 구간·새 키는 더해질 수 있음).
    public static List<string> Diff(Ledger oldLedger, Ledger newLedger)
    {
        var problems = new List<string>();
        for (var i = 0; i < oldLedger.Slots.Count; i++)
        {
            var o = oldLedger.Slots[i];
            if (i >= newLedger.Slots.Count || newLedger.Slots[i].Dungeon != o.Dungeon)
            {
                problems.Add($"구간 순서가 바뀌었거나 '{o.Dungeon}'이 사라졌습니다.");
                continue;
            }
            var n = newLedger.Slots[i];
            if (!string.Equals(n.Source, o.Source, StringComparison.OrdinalIgnoreCase)) problems.Add($"{o.Dungeon}: 원본이 {o.Source} → {n.Source}로 바뀌었습니다.");
            if (n.Base != o.Base) problems.Add($"{o.Dungeon}: 구간 시작이 바뀌었습니다.");
            if (ParseId(n.Next) < ParseId(o.Next)) problems.Add($"{o.Dungeon}: next가 줄었습니다({o.Next} → {n.Next}).");
            foreach (var (k, v) in o.Ids)
            {
                if (!n.Ids.TryGetValue(k, out var nv)) problems.Add($"{o.Dungeon}: 키 {k}가 사라졌습니다.");
                else if (nv != v) problems.Add($"{o.Dungeon}: 키 {k}의 번호가 {v} → {nv}로 바뀌었습니다.");
            }
        }
        return problems;
    }

    public static string SourceKey(Mutagen.Bethesda.Plugins.FormKey k) => $"{k.ModKey.FileName}:{k.ID:X6}";

    public static string EntryKey(Mutagen.Bethesda.Plugins.FormKey source, string role) => $"{SourceKey(source)}:{role}";

    private static bool IsSourceKey(string s)
    {
        var i = s.LastIndexOf(':');
        return i > 0 && s.Length - i - 1 == 6 && s[(i + 1)..].All(Uri.IsHexDigit);
    }

    private static bool IsEntryKey(string k)
    {
        var i = k.LastIndexOf(':');
        return i > 0 && k[(i + 1)..] is "cell" or "ref" or "anchor" && IsSourceKey(k[..i]);
    }

    public static uint ParseId(string s)
    {
        var t = s.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? s[2..] : s;
        return Convert.ToUInt32(t, 16);
    }

    public static string Format(uint id) => $"0x{id:X6}";
}

internal sealed class LedgerSlot
{
    [JsonRequired] public string Dungeon { get; set; } = "";
    // 이 구간이 묶인 원본 셀(예: "Skyrim.esm:0B6BE6")
    [JsonRequired] public string Source { get; set; } = "";
    [JsonRequired] public string Base { get; set; } = "";
    [JsonRequired] public uint Size { get; set; }
    [JsonRequired] public string Next { get; set; } = "";
    [JsonRequired] public SortedDictionary<string, string> Ids { get; set; } = new(StringComparer.Ordinal);
}

internal sealed class LedgerChangeRequiredException : Exception
{
    public LedgerChangeRequiredException(string message) : base(message) { }
}

internal static class AtomicFile
{
    public static void WriteText(string path, string text)
    {
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, text, new System.Text.UTF8Encoding(false));
        File.Move(tmp, path, overwrite: true);
    }
}
