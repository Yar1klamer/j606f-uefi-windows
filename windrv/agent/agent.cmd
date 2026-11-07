@echo off
rem J606F agent: started by the j606fagent service (LocalSystem) at every boot. agent_main.cmd is detached, so the SCM start timeout does not matter.
if not exist C:\j606f\out mkdir C:\j606f\out
start "" /b cmd /c C:\j606f\agent_main.cmd
