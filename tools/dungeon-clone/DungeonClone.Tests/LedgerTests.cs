// FormID 장부 규칙 시험(6단계 재작업 계획 R1 2번): 판이 바뀌어도 남은 참조의 번호가 그대로인지, 장부가 규칙을 지키는지.
using SkyrimPoE.Tools.DungeonClone;
using Xunit;

namespace DungeonClone.Tests;

public class LedgerTests
{
    private const string SrcA = "Skyrim.esm:000A00";
    private const string SrcB = "Skyrim.esm:000B00";

    private static string Ref(uint id) => $"Skyrim.esm:{id:X6}:ref";

    private static Ledger RoundTrip(Ledger ledger)
    {
        var path = Path.Combine(Path.GetTempPath(), $"ledger-{Guid.NewGuid():N}.json");
        try
        {
            ledger.Save(path);
            return Ledger.Load(path, createIfMissing: false);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void SlotsAreAppendedInFirstSeenOrder()
    {
        var l = new Ledger();
        Assert.Equal("0x000800", l.GetSlot("a", SrcA, true).Base);
        Assert.Equal("0x002800", l.GetSlot("b", SrcB, true).Base);
        Assert.Equal("0x000800", l.GetSlot("a", SrcA, true).Base);
    }

    [Fact]
    public void SameKeyKeepsSameIdAcrossBuilds()
    {
        var l = new Ledger();
        var s = l.GetSlot("d", SrcA, true);
        var c = l.Assign(s, "Skyrim.esm:000A00:cell", true);
        var r1 = l.Assign(s, Ref(1), true);
        var reloaded = RoundTrip(l);
        var s2 = reloaded.GetSlot("d", SrcA, false);
        Assert.Equal(c, reloaded.Assign(s2, "Skyrim.esm:000A00:cell", false));
        Assert.Equal(r1, reloaded.Assign(s2, Ref(1), false));
        Assert.Equal(0, reloaded.NewAssignments);
    }

    [Fact]
    public void RemovedRefIdIsNotReusedAndOthersStay()
    {
        var l = new Ledger();
        var s = l.GetSlot("d", SrcA, true);
        var a = l.Assign(s, Ref(0xA), true);
        var b = l.Assign(s, Ref(0xB), true);
        var c = l.Assign(s, Ref(0xC), true);

        // 2판: B가 빠지고 D가 새로 생기고 순서가 바뀜
        var l2 = RoundTrip(l);
        var s2 = l2.GetSlot("d", SrcA, true);
        var d = l2.Assign(s2, Ref(0xD), true);
        Assert.Equal(c, l2.Assign(s2, Ref(0xC), true));
        Assert.Equal(a, l2.Assign(s2, Ref(0xA), true));
        Assert.NotEqual(b, d);
        Assert.Equal(c + 1, d);
        Assert.Equal(1, l2.NewAssignments);
    }

    [Fact]
    public void NewDungeonAtEndDoesNotMoveExistingDungeons()
    {
        var l = new Ledger();
        var x = l.Assign(l.GetSlot("x", SrcA, true), "Skyrim.esm:000A00:cell", true);
        var l2 = RoundTrip(l);
        var y = l2.Assign(l2.GetSlot("y", SrcB, true), "Skyrim.esm:000B00:cell", true);
        Assert.Equal(x, l2.Assign(l2.GetSlot("x", SrcA, false), "Skyrim.esm:000A00:cell", false));
        Assert.Equal(x + Ledger.SlotSize, y);
    }

    [Fact]
    public void SourceIsBoundToKey()
    {
        var l = new Ledger();
        l.GetSlot("x", SrcA, true);
        // 같은 키에 다른 원본: 갱신을 허락해도 실패
        Assert.Throws<InvalidDataException>(() => l.GetSlot("x", SrcB, true));
        // 같은 원본에 다른 키: 실패(번호가 모두 바뀜)
        Assert.Throws<InvalidDataException>(() => l.GetSlot("y", SrcA, true));
    }

    [Fact]
    public void UnknownKeyWithoutUpdateFails()
    {
        var l = new Ledger();
        var s = l.GetSlot("d", SrcA, true);
        l.Assign(s, "Skyrim.esm:000A00:cell", true);
        Assert.Throws<LedgerChangeRequiredException>(() => l.Assign(s, Ref(1), false));
        Assert.Throws<LedgerChangeRequiredException>(() => l.GetSlot("other", SrcB, false));
        Assert.Throws<ArgumentException>(() => l.Assign(s, "nonsense", true));
    }

    [Fact]
    public void SlotOverflowFails()
    {
        var l = new Ledger();
        var s = l.GetSlot("d", SrcA, true);
        for (uint i = 0; i < Ledger.SlotSize; i++)
        {
            l.Assign(s, Ref(i), true);
        }
        Assert.Throws<InvalidDataException>(() => l.Assign(s, Ref(0xFFFFFF), true));
    }

    [Fact]
    public void ValidateRejectsBrokenLedgers()
    {
        // 번호 겹침
        var dup = new Ledger();
        var s = dup.GetSlot("d", SrcA, true);
        dup.Assign(s, Ref(1), true);
        dup.Assign(s, Ref(2), true);
        s.Ids[Ref(2)] = s.Ids[Ref(1)];
        Assert.Throws<InvalidDataException>(() => dup.Validate());

        // 지운 키(밀도 위반)
        var hole = new Ledger();
        var h = hole.GetSlot("d", SrcA, true);
        hole.Assign(h, Ref(1), true);
        hole.Assign(h, Ref(2), true);
        hole.Assign(h, Ref(3), true);
        h.Ids.Remove(Ref(2));
        Assert.Throws<InvalidDataException>(() => hole.Validate());

        // 구간 틈
        var gap = new Ledger();
        gap.GetSlot("a", SrcA, true);
        gap.GetSlot("b", SrcB, true).Base = "0x004800";
        Assert.Throws<InvalidDataException>(() => gap.Validate());

        // 같은 원본 두 구간
        var twice = new Ledger();
        twice.GetSlot("a", SrcA, true);
        twice.Slots.Add(new LedgerSlot { Dungeon = "b", Source = SrcA, Base = "0x002800", Size = Ledger.SlotSize, Next = "0x002800" });
        Assert.Throws<InvalidDataException>(() => twice.Validate());
    }

    [Fact]
    public void DiffCatchesChangesToPublishedLedger()
    {
        var old = new Ledger();
        var s = old.GetSlot("d", SrcA, true);
        old.Assign(s, Ref(1), true);
        old.Assign(s, Ref(2), true);
        var oldCopy = RoundTrip(old);

        // 정상: 새 키만 더함
        var grown = RoundTrip(old);
        grown.Assign(grown.GetSlot("d", SrcA, true), Ref(3), true);
        Assert.Empty(Ledger.Diff(oldCopy, grown));

        // 맞바꿈
        var swapped = RoundTrip(old);
        var ss = swapped.Slots[0];
        (ss.Ids[Ref(1)], ss.Ids[Ref(2)]) = (ss.Ids[Ref(2)], ss.Ids[Ref(1)]);
        Assert.NotEmpty(Ledger.Diff(oldCopy, swapped));

        // 되돌림(마지막 키와 next를 함께 지움): 장부 하나로는 밀도 검사를 통과하지만 diff로 잡힌다
        var rolled = RoundTrip(old);
        rolled.Slots[0].Ids.Remove(Ref(2));
        rolled.Slots[0].Next = Ledger.Format(Ledger.ParseId(rolled.Slots[0].Next) - 1);
        rolled.Validate();
        Assert.NotEmpty(Ledger.Diff(oldCopy, rolled));
    }
}
