param(
    [string]$ConfigPath = '',
    [string]$PluginKey = 'desktop-control@created-by-me-remote'
)
$ErrorActionPreference = 'Stop'
if (-not $ConfigPath) {
    $configRoot = if ($env:CODEX_HOME) { $env:CODEX_HOME } else { Join-Path $env:USERPROFILE '.codex' }
    $ConfigPath = Join-Path $configRoot 'config.toml'
}
if ($PluginKey -notmatch '^desktop-control(?:@[a-z0-9-]+)?$') { throw 'Expected a Desktop Control plugin key' }
function Set-SectionValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
    # Update one TOML scalar; Text is the file, Section is a dotted table, Key/Value define its setting.
    $header = '[' + $Section + ']'
    $match = [regex]::Match($Text, '(?m)^' + [regex]::Escape($header) + '[ \t]*\r?$')
    $newLine = $Key + ' = "' + $Value + '"'
    if (-not $match.Success) { return $Text.TrimEnd() + "`n`n" + $header + "`n" + $newLine + "`n" }
    $start = $match.Index + $match.Length
    $next = [regex]::Match($Text.Substring($start), '(?m)^\[')
    $end = if ($next.Success) { $start + $next.Index } else { $Text.Length }
    $body = $Text.Substring($start, $end - $start)
    $keyPattern = '(?m)^[ \t]*' + [regex]::Escape($Key) + '[ \t]*=.*$'
    if ([regex]::IsMatch($body, $keyPattern)) { $body = [regex]::Replace($body, $keyPattern, $newLine) }
    else { $body = "`n" + $newLine + "`n" + $body.TrimStart() }
    return $Text.Substring(0, $start) + $body + $Text.Substring($end)
}
$before = if (Test-Path -LiteralPath $ConfigPath) { [IO.File]::ReadAllText($ConfigPath) } else { '' }
$after = $before
$keys = @($PluginKey, 'desktop-control') | Select-Object -Unique
foreach ($key in $keys) {
    $section = 'plugins."' + $key + '".mcp_servers."desktop-control"'
    $after = Set-SectionValue $after $section 'default_tools_approval_mode' 'approve'
}
if ($after -ne $before) {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($ConfigPath))) -Force | Out-Null
    if (Test-Path -LiteralPath $ConfigPath) { Copy-Item -LiteralPath $ConfigPath -Destination ($ConfigPath + '.desktop-control-backup-' + [DateTime]::Now.ToString('yyyyMMddHHmmss')) }
    [IO.File]::WriteAllText($ConfigPath, $after, [Text.UTF8Encoding]::new($false))
}
Write-Output 'Desktop Control tools approved for this plugin only. Initial server session consent remains required. Restart Codex or open a new chat.'
