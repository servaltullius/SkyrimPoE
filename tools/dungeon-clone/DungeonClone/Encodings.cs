// 지도 팩 ESP의 문자열 인코딩. 코어 플러그인(esp/SkyrimPoE.esp)과 같이 한글 이름을 UTF-8로 쓰고 읽는다.
// Mutagen의 기본(서유럽 코드 페이지)으로 쓰면 한글 셀 이름이 깨진다(2026-11-22 검증기에서 확인).
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Plugins.Binary.Streams;
using Mutagen.Bethesda.Strings;
using Mutagen.Bethesda.Strings.DI;

namespace SkyrimPoE.Tools.DungeonClone;

internal static class Encodings
{
    public static readonly BinaryWriteParameters WriteParameters = new()
    {
        Encodings = new EncodingBundle(MutagenEncoding._utf8, MutagenEncoding._utf8),
    };

    public static readonly BinaryReadParameters ReadParameters = new()
    {
        StringsParam = new StringsReadParameters
        {
            NonTranslatedEncodingOverride = MutagenEncoding._utf8,
            NonLocalizedEncodingOverride = MutagenEncoding._utf8,
        },
    };
}
