[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function New-DeterministicZip {
    param(
        [Parameter(Mandatory)]
        [string] $SourceDirectory,
        [Parameter(Mandatory)]
        [string] $DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression

    if ( Test-Path -LiteralPath $DestinationPath ) {
        Remove-Item -LiteralPath $DestinationPath -Force
    }

    $SourceRoot = ( Resolve-Path -LiteralPath $SourceDirectory ).Path
    $ArchiveStream = [System.IO.File]::Open(
        $DestinationPath,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None
    )
    $Archive = [System.IO.Compression.ZipArchive]::new(
        $ArchiveStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false
    )

    try {
        $ArchiveTimestamp = [System.DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [System.TimeSpan]::Zero)
        $Files = Get-ChildItem -LiteralPath $SourceRoot -File -Recurse | Sort-Object {
            [System.IO.Path]::GetRelativePath($SourceRoot, $_.FullName).Replace('\', '/')
        }

        foreach ( $File in $Files ) {
            $RelativePath = [System.IO.Path]::GetRelativePath($SourceRoot, $File.FullName).Replace('\', '/')
            $Entry = $Archive.CreateEntry($RelativePath, [System.IO.Compression.CompressionLevel]::Optimal)
            $Entry.LastWriteTime = $ArchiveTimestamp

            $InputStream = [System.IO.File]::OpenRead($File.FullName)
            $OutputStream = $Entry.Open()
            try {
                $InputStream.CopyTo($OutputStream)
            } finally {
                $OutputStream.Dispose()
                $InputStream.Dispose()
            }
        }
    } finally {
        $Archive.Dispose()
        $ArchiveStream.Dispose()
    }
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"
    $SymbolsOutputName = "${OutputName}-symbols"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $InstallItems = @(
        Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*"
    )
    if ( $InstallItems.Count -eq 0 ) {
        throw "No installed plugin files found for ${Configuration}."
    }

    $ArchiveStagingRoot = Join-Path -Path ([System.IO.Path]::GetTempPath()) -ChildPath "obs-plugin-package-$([guid]::NewGuid().ToString('N'))"
    $SymbolsStagingRoot = Join-Path -Path ([System.IO.Path]::GetTempPath()) -ChildPath "obs-plugin-symbols-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType 'Directory' -Path $ArchiveStagingRoot | Out-Null
    New-Item -ItemType 'Directory' -Path $SymbolsStagingRoot | Out-Null

    try {
        Copy-Item -LiteralPath $InstallItems.FullName -Destination $ArchiveStagingRoot -Recurse

        $PackageRoot = Join-Path -Path $ArchiveStagingRoot -ChildPath $ProductName
        if ( ! ( Test-Path -LiteralPath $PackageRoot -PathType Container ) ) {
            throw "Installed plugin root not found: ${PackageRoot}"
        }
        Copy-Item -LiteralPath "${ProjectRoot}/README.md" -Destination $PackageRoot
        Copy-Item -LiteralPath "${ProjectRoot}/LICENSE" -Destination $PackageRoot

        $PluginPdb = Join-Path -Path $PackageRoot -ChildPath "bin/64bit/${ProductName}.pdb"
        if ( ! ( Test-Path -LiteralPath $PluginPdb -PathType Leaf ) ) {
            throw "Plugin debug symbols not found: ${PluginPdb}"
        }

        $SymbolsPackageRoot = Join-Path -Path $SymbolsStagingRoot -ChildPath $SymbolsOutputName
        New-Item -ItemType 'Directory' -Path $SymbolsPackageRoot | Out-Null
        Copy-Item -LiteralPath $PluginPdb -Destination $SymbolsPackageRoot
        Copy-Item -LiteralPath "${ProjectRoot}/README.md" -Destination $SymbolsPackageRoot
        Copy-Item -LiteralPath "${ProjectRoot}/LICENSE" -Destination $SymbolsPackageRoot

        Get-ChildItem -LiteralPath $ArchiveStagingRoot -Filter '*.pdb' -File -Recurse -Force | Remove-Item -Force
        if ( Get-ChildItem -LiteralPath $ArchiveStagingRoot -Filter '*.pdb' -File -Recurse -Force ) {
            throw 'The main Windows package still contains debug symbols.'
        }

        New-DeterministicZip `
            -SourceDirectory $ArchiveStagingRoot `
            -DestinationPath "${ProjectRoot}/release/${OutputName}.zip"
        New-DeterministicZip `
            -SourceDirectory $SymbolsStagingRoot `
            -DestinationPath "${ProjectRoot}/release/${SymbolsOutputName}.zip"
    } finally {
        Remove-Item -LiteralPath $ArchiveStagingRoot -Recurse -Force -ErrorAction 'SilentlyContinue'
        Remove-Item -LiteralPath $SymbolsStagingRoot -Recurse -Force -ErrorAction 'SilentlyContinue'
    }
    Log-Group
}

Package
