#!/usr/bin/env pwsh

$global:CompilerFlags = @(
    '/FS'
)

$global:LinkerFlags = @(
    '/LTCG'
    '/INCREMENTAL:NO'
    '/OPT:REF'
    '/DEBUG'
    '/PDBALTPATH:%_PDB%'
)

$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio' Installer vswhere

$global:VsInstallPrefix = & $VsWhere -Products '*' -Latest -Property installationPath
$global:VsVersion = ((& $VsWhere -Property catalog_productSemanticVersion -Path $VsInstallPrefix) -Split '[+]')[0]
$global:VcVarsScript = Join-Path $VsInstallPrefix VC Auxiliary Build vcvarsall.bat
