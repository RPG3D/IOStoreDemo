

set EngineDir="E:\Code\UnrealEngine\Engine"

set Bat=%EngineDir%/Build/BatchFiles/RunUAT.bat

set Target=IOStoreDemo

set UProject=%~dp0/../../%Target%.uproject


set TargetPlatform=Win64

set ClientConfig=Development

set Param=BuildCookRun -project=%UProject% -Build ^
-Target=%Target% -TargetPlatform=%TargetPlatform% -ClientConfig=%ClientConfig% ^
-Cook -Pak -Stage

call %Bat% %Param%
