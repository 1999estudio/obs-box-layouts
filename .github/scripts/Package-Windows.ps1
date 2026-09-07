[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [bool] $Installer = $false
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

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs

    if ( $Installer ) {
        $InstallerScript = "${ProjectRoot}/installer/windows/obs-box-layouts.iss"
        $InstallerCompilerCandidates = @(
            "${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe",
            "${env:ProgramFiles}/Inno Setup 6/ISCC.exe"
        )
        $InstallerCompiler = $InstallerCompilerCandidates |
            Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1

        if ( $null -eq $InstallerCompiler ) {
            throw 'Inno Setup 6 was not found. Install it before requesting a Windows installer.'
        }

        Log-Group "Creating Windows installer for ${ProductName}..."
        $InstallerArgs = @(
            "/DMyAppVersion=${ProductVersion}",
            "/DSourceDir=${ProjectRoot}/release/${Configuration}/${ProductName}",
            "/DOutputDir=${ProjectRoot}/release",
            $InstallerScript
        )
        Invoke-External $InstallerCompiler @InstallerArgs
        Log-Group
    }
    Log-Group
}

Package
