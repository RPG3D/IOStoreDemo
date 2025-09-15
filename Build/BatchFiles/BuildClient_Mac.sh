

EngineDir="/Users/admin/Documents/Code/UnrealEngine/Engine"

Bat=$EngineDir/Build/BatchFiles/RunUAT.sh

Target=IOStoreDemo

UProject=$(pwd)/../../$Target.uproject


TargetPlatform=Mac

ClientConfig=Development

Param="BuildCookRun -project=$UProject -Build \
-Target=$Target -TargetPlatform=$TargetPlatform -ClientConfig=$ClientConfig \
-Cook -Pak -Stage"

$Bat $Param

