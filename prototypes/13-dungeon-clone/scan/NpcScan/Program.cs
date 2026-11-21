// R0 조사: 스폰 원형 후보 NPC의 레벨 규칙과 레벨드 목록 항목을 낸다(읽기 전용).
using System.Text.RegularExpressions;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

var data = args.Length > 0 ? args[0] : @"G:\TAKEALOOK\Stock Game\Data";
var pattern = new Regex(args.Length > 1 ? args[1] : "^Lvl", RegexOptions.IgnoreCase);
var extraIds = args.Skip(2).Select(a => FormKey.Factory($"{a}:Skyrim.esm")).ToHashSet();
using var mod = SkyrimMod.CreateFromBinaryOverlay(Path.Combine(data, "Skyrim.esm"), SkyrimRelease.SkyrimSE);
var cache = mod.ToImmutableLinkCache();

string Lv(INpcGetter n)
{
    var c = n.Configuration;
    var lvl = c.Level switch
    {
        INpcLevelGetter f => $"fixed {f.Level}",
        IPcLevelMultGetter m => $"PCx{m.LevelMult:0.##} [{c.CalcMinLevel}-{(c.CalcMaxLevel == 0 ? "inf" : c.CalcMaxLevel.ToString())}]",
        _ => "?",
    };
    return lvl;
}

string Describe(FormKey k)
{
    if (!cache.TryResolve(k, out IMajorRecordGetter? r)) return k.ToString();
    return r switch
    {
        INpcGetter n => $"{n.EditorID}({Lv(n)})",
        ILeveledNpcGetter l => $"LVLN {l.EditorID}",
        _ => r.EditorID ?? k.ToString(),
    };
}

foreach (var n in mod.Npcs)
{
    if (!(extraIds.Contains(n.FormKey) || (n.EditorID is { } e && pattern.IsMatch(e)))) continue;
    var line = $"{n.FormKey.ID:X6} {n.EditorID} | {Lv(n)}";
    if (n.Template is { IsNull: false } t && cache.TryResolve(t.FormKey, out IMajorRecordGetter? tr))
    {
        line += $" | template {Describe(t.FormKey)}";
        if (tr is ILeveledNpcGetter ll && ll.Entries is { } ents)
        {
            var parts = ents.Where(x => x.Data is not null)
                .Select(x => $"L{x.Data!.Level}:{Describe(x.Data.Reference.FormKey)}").Take(12);
            line += " | " + string.Join("; ", parts);
        }
    }
    Console.WriteLine(line);
}
