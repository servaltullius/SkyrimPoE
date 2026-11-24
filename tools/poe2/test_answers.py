"""Check hand-calculated examples against the committed answer artifact."""
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answers import ROOT


def test_hand_calculated_cases():
    answers = json.loads((ROOT / "data/poe2/answers/p2b.json").read_bytes())
    conversion = {case["id"]: case["expected"] for case in answers["conversion"]}
    for level in (1, 20, 50, 80, 100):
        assert conversion[f"physical-profile-lv{level}"] == {"normalizedPass": 1}
        assert conversion[f"expected-life-lv{level}"] == {"skyrimHealth": 123}
    assert conversion["offense-life-lv1"] == {"referenceLife": 75}
    for level, reference_life in ((20, 305.573), (50, 979.030), (80, 3413.118), (100, 4586.911)):
        assert round(conversion[f"offense-life-lv{level}"]["referenceLife"], 3) == reference_life
    assert conversion["es-difficulty-partial"] == {
        "esRemaining": 0, "absorbedEffectiveDamage": 45, "remainingRawDamage": 5
    }
    pob = {case["id"]: case["expected"] for case in answers["pob"]}
    project = {case["id"]: case["expected"] for case in answers["project"]}
    game = {case["id"]: case["expected"] for case in answers["gameRule"]}
    assert pob["es-chaos-overflow"] == {"esRemaining": 0, "lifeLost": 20}
    assert pob["critical-player-base"] == {"damage": 200}
    assert pob["armour-negative-tie"] == {"reductionPercent": -2}
    assert pob["monster-hit-lv1-ev100"] == {"monsterAccuracy": 32, "hitChancePercent": 58}
    assert project["spirit-mask-5"] == {"spirit": 170}
    assert project["spirit-mask-15"] == {"spirit": 230}
    assert project["xp-lv20-enemy21"] == {"multiplier": 1.1}
    assert game["es-poison-bypass"] == {"esRemaining": 60, "lifeLost": 50}
    assert game["es-ignite-absorbed"] == {"esRemaining": 10, "lifeLost": 0, "secondsSinceEsLoss": 0}
    assert game["es-ignite-overflow"] == {"esRemaining": 0, "lifeLost": 20, "secondsSinceEsLoss": 0}
    assert game["chill-game-290"] == {"effectPercent": 0, "applies": False}
    assert game["chill-game-300"] == {"effectPercent": 30, "applies": True}
    assert game["chill-game-600"] == {"effectPercent": 50, "applies": True}
    assert game["ignite-flammability-enemy"] == {"chancePercentByHit": [20, 80, 100, 100, 100, 60, 40]}
    assert game["ignite-flammability-player"] == game["ignite-flammability-enemy"]
    assert pob["poison-base-stacks"] == {"activeStacks": 1, "damagePerSecond": 40, "durationSeconds": 2}
    assert pob["dot-bleed-aggravated"] == {"damagePerSecond": 30, "durationSeconds": 5}
    assert pob["thresholds-lv80"]["ailmentThreshold"] == 65527
    assert pob["thresholds-lv80"]["poiseThreshold"] == 213635
    steps = game["freeze-game-buildup"]["steps"]
    # The source tables give a full-poise threshold but no supported repeat modifier timing.
    # The 13 s hit freezes and clears buildup. The 23 s hit starts fresh and builds 84%.
    assert [round(step["buildupPercent"], 9) for step in steps] == [63, 63, 58, 0, 0, 0, 84]
    assert [step["frozen"] for step in steps] == [False, False, False, True, True, True, False]
