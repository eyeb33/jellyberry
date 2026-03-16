$file = "d:\Jelly\Git\jellyberry\server\main.ts"
$content = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)

# Three mojibake sequences present in the file.
# Each is a UTF-8 multibyte sequence (E2 xx xx) that was saved through a
# Windows-1252 encoding step, so each real character became 3 Latin-1 codepoints.
#
# UTF-8 bytes  -> Windows-1252 codepoints   -> correct Unicode char
# E2 80 94     -> U+00E2 U+20AC U+201D      -> U+2014 — (em dash)
# E2 86 92     -> U+00E2 U+2020 U+2019      -> U+2192 → (rightwards arrow)
# E2 94 80     -> U+00E2 U+201D U+20AC      -> U+2500 ─ (box drawings light horizontal)

$replacements = @(
    @{ From = [char]0x00E2 + [char]0x20AC + [char]0x201D; To = [char]0x2014 }  # — em dash
    @{ From = [char]0x00E2 + [char]0x2020 + [char]0x2019; To = [char]0x2192 }  # → right arrow
    @{ From = [char]0x00E2 + [char]0x201D + [char]0x20AC; To = [char]0x2500 }  # ─ box line
)

$before = $content.Length
foreach ($r in $replacements) {
    $count = 0
    $idx = 0
    while (($idx = $content.IndexOf($r.From, $idx)) -ge 0) { $count++; $idx++ }
    Write-Host "  '$($r.From)' -> '$($r.To)' : $count occurrences"
    $content = $content.Replace($r.From, $r.To)
}

[System.IO.File]::WriteAllText($file, $content, [System.Text.Encoding]::UTF8)

# Verify no mojibake remains
$verify = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)
$remaining = (Select-String -InputObject $verify -Pattern 'â€|â†|â"' -AllMatches).Matches.Count
Write-Host ""
Write-Host "File length: $before -> $($verify.Length) chars"
Write-Host "Remaining mojibake matches: $remaining"
if ($remaining -eq 0) { Write-Host "SUCCESS: File is clean" } else { Write-Host "WARNING: Some sequences may remain" }
