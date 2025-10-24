

set EngineDir=E:\Code\UE5\Engine

set Bat=%EngineDir%/Build/BatchFiles/RunUAT.bat

set Target=IOStoreDemo

set UProject=%~dp0/../../%Target%.uproject


set TargetPlatform=Win64

set ClientConfig=Development

set Param=BuildCookRun -project=%UProject% -Build -Cook -Pak -Stage -CreateReleaseVersion=1.0.0 ^
-Target=%Target% -TargetPlatform=%TargetPlatform% -ClientConfig=%ClientConfig%

call %Bat% %Param%
