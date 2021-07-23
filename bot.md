# Bot
## Why bot?
Hi. I'm kaddy aka Lite. I played Teeworlds for years (since ~2008), wrote several client mods (the most known is Z-Team Pack) and bots. But time goes, I grown older and have not much free time anymore, and Teeworlds is almost dead nowadays (or better to say - it turned into DDRace)...
Couple weeks ago I accidentally found my old bot source codes. So I cloned Teeworlds repository, put bot code inside, fixed it a bit (because it was made for 0.6) and... it was funny! But the code was really mess, so I decided to re-write it from zero.
This brand-new bot actually is not the best one. It moves really fast (for a bot, of course), but shoots so-so, chooses non-optimal destinations and sometimes can stuck. I am okay with current progress and don't really want to finish it, but would be really appreciated if someone will improve my code.

### What it can do
Bot can play DM, TDM and CTF modes.
It can kill low and medium skilled players (but pro can kill it easily).
It can carry flags, but sometimes may stuck.

### What it cannot do
Play other game modes.
Play like a pro.
Play good on open maps (like `ctf5`).
Use hammer.

## Pathfinding
### 1. Edges finding
At first, bot inspects the map and tries to find edges on it. It's hard to explain how it works, but it works quite good.
Methods: `CBot::FindEdges()`.
Example for `dm1`:
```
............................................................
............................................................
..............................X X...........................
...XX      XX.................   ...........................
...X          XX.............X   X....................XX X..
...            XX.XX              XX.................XX   ..
...                                  XX.............XX    ..
...                                   XX......XX          ..
...                                                       ..
...                         XX..X                         ..
...X                  X..........                         ..
...X                  X..........X                        ..
....                      X............X     X......X     ..
....X                      X............     X.......     ..
..................X            XX......X     X.......     ..
...................             XX.XX         X.....X    X..
...X        XX....X                                      X..
...          XX..XX                                      ...
...                                                      ...
...                                                     X...
...                                              X..........
...X                                             X..........
......XX                X................X          XX......
.......X                ..................           XX.....
........                ..X       XX.....X               X..
........                ..                                ..
........       X...X    ..                                ..
........       X...X    ..X                               ..
........                .....X                            ..
........X               X.....                           X..
..........X               XX..X           XX.X           X..
..........X                X.................X           ...
........XX                  X................X           ...
...X                            XX..........X           X...
...              XX                                 X.......
...              ..                                 ........
...             X..                                 ........
...X   XX..........X                               X........
..........................X                X................
...........................                .................
...........................                .................
...........................X              X.................
............................................................
............................................................
............................................................
............................................................
............................................................
............................................................
............................................................
............................................................
```
### 2. Triangulation
When all edges found the bot tries to find triangles between them, using Delaunau triangulation algorith.
The original algorith is quite simple:
1. take every 3 edges as triangle (`CBotTriangle`);
2. find circumcenter for this triangle (`CBotTriangle::CalculateCircumcenter()`):
![Triangle circumcenter](https://upload.wikimedia.org/wikipedia/commons/7/74/Triangle.Circumcenter.svg)
3. check if there are another edges inside of the circle;
3.1. if there are any other edges, throw this triangle out;
3.2. if there are no edges more, save this triangle.

But I modified it slightly, under pt. 3. Bot looks only for edges that are visible
After all triangles found, bot find neighbors for each one. Neighbor is a triangle which has 2 same points to current one's.

Methods: `CBot::TriangulateMap(), CBot::FindTrianglesNeighbors()`.

This is quite long process, especially for large maps like `ctf2` or `ctf5`. That's why triangles are being cached into files, in methods `CBot::SaveTriangles()` and `CBot::LoadTriangles()`.

Why do we need triangulation? To use triangles' geometric center (`CBotTriangle.m_Center`) as waypoints.

### 3. A*
It's well-known pathfinding algorith. I guess [Wikipedia](https://en.wikipedia.org/wiki/A*_search_algorithm) can explain it better than me.
Bot already knows own destination, where it should walk to.
First it checks if it can "see" the destination directly and if there's no walls between. In this case bot uses straigh line as path, and no need to use A*.
In other case, it finds 2 triangles: at current bot's position and at destination (`CBot::FindTriangle()`, `CBotTriangle::IsInside()`). Then it finds path using classic A* algorith. After the path is found, bot adds current position and destination points to it, and returns as result.
Because pathfinding is quite heavy operation (especially on large maps), A* results are being cached (`CBot.m_PathCache`).
Would be nice if the bot could save this cache into file or compute pathes from every triangle to every other on idle.

Also, if current position or destination aren't inside of any triangle, bot tries to find nearest visible triangle (with no collisions from position to triangle center).

Methods: `CBot::FindPath()`.

### 4. Input prediction
Ok, we know the way, so what? There are jumps, hooks etc. - how to make the bot follow the path?
Original Teeworlds bot (by @matricks, I guess?), the most first one I know, used A* for that, because it's really universal algorith. But it computed it's movements so slowly so you could drink couple cups of coffee waiting when it finally reach the destination.
What my bot does...
It predicts all possible movements for several ticks: move left, stay, more right, move left and jump, just jump, move right and jump, and so on. It also tries to use hook on current triangle's edges and to random position. For each movement it calculates path distance, from it's predicted position to the destination.
After several iterations bot checks which movement has minimal path distance, and returns this movement's initial input as result.

Methods: `CBot::Move()`, `CBotMoveAttempt::Predict()`.

### Pros and cons
The bot moves really fast, rarely stuck on edges. Calculations doesn't take lot of time.
As big minus, sometimes it can't find best way. For example, at `ctf5` it constantly tries to jump up to the laser and falling down, unable to reach it.

### Screenshot
![Screenshot](https://i.ibb.co/3y92nYx/screenshot-2021-07-23-13-13-24.png)
Gray rectangles are map edges (`CBot::RenderEdges()`).
White lines are triangles (`CBot::RenderTriangles()`).
Red line is a pathway to a hearth into the cave (`CBot::RenderPath()`).

## Destination choosing
Bot looks around for available entities: ammo, health, shields, flags, enemies. It adds flags at stands, if it's CTF game.
After that it chooses where it should move next, using different weights for each entity.
See `CBot::ChooseDestination()` for details.

## Shooting
That's quite simple. Bot predicts own movement for an one step (because we shoot not right now, but when server can process our input data) and checks what weapons it has. Then looks around in 360 degrees and predicts movements of an projectile. If some projectile intersects with predicted enemy position, it shoots.