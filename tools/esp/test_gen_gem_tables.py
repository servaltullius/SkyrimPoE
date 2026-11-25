from gen_gem_tables import OUTPUT, render


def test_generated_gem_header_is_current():
    assert OUTPUT.read_bytes() == render()
