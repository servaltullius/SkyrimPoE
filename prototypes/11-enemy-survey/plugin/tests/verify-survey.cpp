#include "Survey.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
    void Check(bool pass, std::string_view message)
    {
        if (!pass) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }

    void CheckDefaultConfig(const char* path)
    {
        std::ifstream input(path, std::ios::binary);
        Check(input.is_open(), "배포 INI 열기");
        const auto config = Survey::ReadConfig(input, [](std::string_view warning) { Check(false, warning); });
        Check(config.CanRun() && config.levels == std::vector{ 1, 10, 20, 30, 50 }, "기본 레벨과 실행 조건");
        Check(config.cells.size() == 16 && config.cells.front().editorId == "BleakFallsBarrow01" &&
            config.cells.front().localId == 0x0371DE && config.cells.back().editorId == "FaldarsTooth01" &&
            config.cells.back().localId == 0x015286, "기본 셀과 순서");
        Check(config.mainMenuDelay.count() == 3 && config.startDelay.count() == 5 && config.settle.count() == 4 &&
            config.cellTimeout.count() == 90 && config.quitWhenDone && config.notify, "기본 지연과 스위치");
    }

    void CheckInvalidConfig()
    {
        std::istringstream input(
            "\xEF\xBB\xBF[Survey]\r\n"
            "Levels = 81, 0, x, 20, 82, -1, , 1, 20\r\n"
            "Cells = Valid_2: Bcd : Test.ESM, Missing, Bad:ZZ:Test.esm, TooLarge:1000000:Core.esm, "
            "Bad Plug:09:Core.esm, Unsafe:9:../Core.esm, Valid_2:1:Core.esm, Same:0BCD:test.esm, "
            "Other:1a2b:Extra Plugin.esl\r\n"
            "MainMenuDelaySeconds = 0\r\n"
            "StartDelaySeconds = -1\r\n"
            "SettleSeconds = 12\r\n"
            "CellTimeoutSeconds = 0\r\n"
            "QuitWhenDone = FaLSe\r\n"
            "Notify = maybe\r\n"
            "bad line\r\n"
            "[Other]\r\nLevels = 5\r\n");
        std::vector<std::string> warnings;
        const auto warn = [&](std::string_view text) { warnings.emplace_back(text); };
        const auto config = Survey::ReadConfig(input, warn);
        Check(config.CanRun() && config.levels == std::vector{ 81, 20, 1, 20 }, "잘못된 레벨만 빼고 중복과 순서 유지");
        Check(config.cells.size() == 2 && config.cells[0].editorId == "Valid_2" && config.cells[0].localId == 0xBCD &&
            config.cells[1].editorId == "Other" && config.cells[1].localId == 0x1A2B &&
            config.cells[1].plugin == "Extra Plugin.esl", "잘못된 셀만 제외");
        Check(config.mainMenuDelay.count() == 0 && config.startDelay.count() == 5 && config.settle.count() == 12 &&
            config.cellTimeout.count() == 90 && !config.quitWhenDone && config.notify, "잘못된 지연과 스위치에 기본값 사용");
        Check(warnings.size() == 16 && warnings.front().find("[0]") != std::string::npos &&
            warnings.back().find("키=값") != std::string::npos, "틀린 항목마다 이유 기록");

        std::istringstream empty("[Survey]\nLevels = 191\nCells = One:12:Test.esm\n");
        Check(!Survey::ReadConfig(empty, warn).CanRun(), "유효한 레벨이나 셀이 부족하면 중단");
        std::istringstream onlyOne("[Survey]\nLevels = 1\nCells = One:12:Test.esm\n");
        Check(!Survey::ReadConfig(onlyOne, warn).CanRun(), "셀 한 개면 중단");
        std::istringstream noLevel("[Survey]\nCells = One:12:Test.esm, Two:13:Test.esm\n");
        Check(!Survey::ReadConfig(noLevel, warn).CanRun(), "레벨 목록이 없으면 중단");
    }

    void CheckOrderAndNames()
    {
        using Queue = std::deque<std::uint32_t>;
        Queue order{ 1, 2, 3 };
        Check(Survey::DeferCurrentCell(order, 1) && order == Queue{ 2, 3, 1 }, "현재 셀만 맨 뒤로 미룸");
        Check(Survey::DeferCurrentCell(order, 3) && order == Queue{ 2, 3, 1 }, "이미 뒤에 있는 현재 셀은 그대로 둠");
        order.pop_front();
        Check(Survey::DeferCurrentCell(order, 3) && order == Queue{ 1, 3 }, "다음 셀이 현재 셀이면 다시 미룸");
        order.pop_front();
        Check(!Survey::DeferCurrentCell(order, 3) && order == Queue{ 3 }, "현재 셀만 남으면 건너뜀");
        Check(Survey::DeferCurrentCell(order, 9), "한 셀이 남아도 다른 셀이면 방문함");

        Check(Survey::ParseMarker("\xEF\xBB\xBF" " Save 11 \r\n") == "Save 11", "UTF-8 BOM, CRLF 표지");
        Check(!Survey::ParseMarker("Save\nSecond") && !Survey::ParseMarker("../Save") && !Survey::ParseMarker("\n"),
            "두 줄, 경로, 빈 표지를 버림");

        std::vector<std::filesystem::path> attempted;
        const auto path = Survey::UniqueLogPath("records", "20261118-080000", [&](const auto& candidate) {
            attempted.push_back(candidate.filename());
            return attempted.size() < 3;
        });
        Check(attempted == std::vector<std::filesystem::path>{ "survey-20261118-080000.tsv", "survey-20261118-080000-2.tsv",
            "survey-20261118-080000-3.tsv" } && path.parent_path() == "records" && path.filename() == attempted.back(), "중복 파일의 번호");
    }

    void CheckRows()
    {
        Check(Survey::Row('S', 0, "start\t0.1.0\t1.6.1170\tSave 11") == "S\t0\tstart\t0.1.0\t1.6.1170\tSave 11\n", "S행");
        Check(Survey::LevelRow(1, 10, 9, 105.5f) == "L\t1\t10\t9\t105.5\n", "L행");
        Check(Survey::VisitRow(2, 10, "Mine", 0xB6BE6, "timeout", 90000, 0) ==
            "V\t2\t10\tMine\t000B6BE6\ttimeout\t90000\t0\n", "V행");
        Check(Survey::ActorRow(3, 10, "Mine", { 0xA123, 0xFF00ABCD, 0xB123, "Skyrim.esm", "산적\t대장\r\n", 0x13746, 12,
            144.25f, 120, 143, 1, "HUX", 150, 20, 15.5f, 0, 0x13135, 5, 12.5f, 6.25f, 0x40008 }) ==
            "A\t3\t10\tMine\t0000A123\tFF00ABCD\t0000B123\tSkyrim.esm\t산적 대장  \t00013746\t12\t144.25\t120\t143\t1\tHUX\t150\t20\t15.5\t0\t00013135\t5\t12.5\t6.25\t00040008\n", "A행");
        Check(Survey::Clean({}) == "-", "빈 기록 필드");
    }
}

int main(int argc, char** argv)
{
    Check(argc == 2, "배포 INI 경로 필요");
    CheckDefaultConfig(argv[1]);
    CheckInvalidConfig();
    CheckOrderAndNames();
    CheckRows();
}
