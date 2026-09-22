chcp 65001 > $null
$env:PYTHONUTF8=1
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py -p COM40 flash monitor
