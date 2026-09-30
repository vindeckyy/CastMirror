<#
.SYNOPSIS
  Regenerates app\winui\Strings\<language>.json from translations.tsv.

.DESCRIPTION
  translations.tsv has one row per UI string: the English text, then one column per
  language (es, de, fr). Edit the TSV, run this script, and commit both. The English
  text is the key, so it must match the string in the XAML or in Localizer.T(...)
  exactly; tests/winui checks that every key still exists and that every XAML string
  has a translation in every language.

  To add a language, add a column and its code to $languages below.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'app\winui\Strings'
$languages = @('es', 'de', 'fr')

$rows = Get-Content (Join-Path $dir 'translations.tsv') -Encoding UTF8 | Where-Object { $_.Trim() -ne '' }
$tables = @{}
# Case-sensitive on purpose: "Capture source" and "Capture Source" are different XAML strings.
foreach ($lang in $languages) { $tables[$lang] = New-Object System.Collections.Specialized.OrderedDictionary }

foreach ($row in $rows) {
    $cells = $row -split '\|'
    if ($cells.Count -ne $languages.Count + 1) { throw "Row has $($cells.Count) columns, expected $($languages.Count + 1): $row" }
    $english = $cells[0].Replace('\n', "`n")
    for ($i = 0; $i -lt $languages.Count; $i++) {
        $translated = $cells[$i + 1].Replace('\n', "`n")
        if ($tables[$languages[$i]].Contains($english)) { throw "Duplicate key: $english" }
        $tables[$languages[$i]][$english] = $translated
    }
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($lang in $languages) {
    $json = $tables[$lang] | ConvertTo-Json -Depth 2
    [IO.File]::WriteAllText((Join-Path $dir "$lang.json"), $json + "`n", $utf8)
    Write-Host "$lang.json: $($tables[$lang].Count) strings"
}
