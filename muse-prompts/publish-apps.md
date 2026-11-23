The GitHub repository for my gadget, dayvson/muse-charm-apps, was reorganised. Please use these steps from now on whenever you build a new Lua app for my gadget or update one. They replace your earlier steps for committing apps. The steps for writing and installing apps on the gadget (luapp.install and friends) are unchanged.

What's in the repository now:
- apps/<id>/: the Lua apps you built, one folder each, the same as before. This is your part of the repository.
- firmware/, docs/, scripts/, patches/, config/, gif-avatar/, muse-prompts/: the gadget's firmware and its docs, which I maintain. Don't change anything there.
- README.md: the project overview, with a table of your apps under "Apps by Muse".
- apps/README.md: the index of your apps, with a table that includes each app's id.

1. Before changing anything, update your copy: git pull --rebase origin main. The repository has new commits you don't have yet.

2. Install on the gadget first, the usual way: read the API with luapp.admin, install with luapp.install (in pieces over 8000 characters), fix any syntax_error or runtime_error, and check luapp.admin logs. Only commit a version that installed with ok and runs.

3. Commit the app:
   - apps/<id>/<id>.lua: the exact source you installed. The folder and file use the app's id in lowercase.
   - apps/<id>/README.md: same style as the others (apps/timer/README.md is a good model). Start with the name as the title and a line saying it runs in the Apps launcher, with its Lua source file and its id. Then sections: What it does, Sound (if any), Installing (luapp.install with its id, name and colour; reinstalling with the same id updates it), and Icon.

4. For a new app, add one row to each table, in the same format as the rows already there:
   - README.md, the table under "Apps by Muse": | [**Name**](apps/<id>/) | one-sentence description |
   - apps/README.md, the table at the top: | [Name](<id>/) | `<id>` | one-sentence description |
   For an update, change those rows only if what the app does has changed. Don't add screenshots or touch the screenshot gallery under the table: I take screenshots on the gadget from my Mac and add them myself.

5. Write commit messages like the earlier ones: the app's name, a colon, and what changed, for example "Weather: hourly panel fits the round face". Commit only files under apps/ and those two README tables. Then git push origin main. If the push is rejected, pull with --rebase again and push again. Never force-push.

6. Tell me the commit and what changed, so I can take the new screenshot.

Please remember these steps for future app requests.
