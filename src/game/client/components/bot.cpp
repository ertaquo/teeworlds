#include <base/math.h>

#include <engine/shared/config.h>

#include <game/collision.h>
#include <game/mapitems.h>
#include <game/client/gameclient.h>
#include <game/client/component.h>

#include <engine/graphics.h>
#include <engine/serverbrowser.h>
#include <engine/storage.h>

extern "C" {
  #include <engine/external/astar-jps/AStar.h>
}

#include "bot.h"
#include "camera.h"

#include <stdio.h>
#include <algorithm>
#include <mutex>
#include <execution>
#include <thread>
#include <queue>
#include <map>
#include <set>

static IGraphics::CTextureHandle gs_EmptyTexture;

CBot::CBot() {
  m_pField = nullptr;
  m_pJPSField = nullptr;

	OnReset();
}

void CBot::OnReset() {
	m_FieldWidth = 0;
	m_FieldHeight = 0;

  if (m_pField != nullptr) {
    delete[] m_pField;
    m_pField = nullptr;
  }
  if (m_pJPSField != nullptr) {
    delete[] m_pJPSField;
    m_pJPSField = nullptr;
  }

  m_aEntities.clear();

  m_TrianglesLock.lock();
  m_TriangulationID.store(0);
  ResetTriangles(m_aTriangles);
  m_bTrianglesReady.store(false);
  m_PathCache.clear();
  m_JPSCache.clear();
  m_TrianglesLock.unlock();

  ResetWeapons();
}

void CBot::OnRelease() {
	OnReset();
}

void CBot::OnRender() {
  if (!Config()->m_ClBot)
    return;

	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	if (m_aEntities.empty()) {
		LoadMapData();
  }

  auto pCharacter = m_pClient->m_Snap.m_pLocalCharacter;
  if (pCharacter) {
    if (pCharacter->m_Weapon != WEAPON_HAMMER) {
      m_aWeapons[pCharacter->m_Weapon] = pCharacter->m_AmmoCount;
    } else {
      m_aWeapons[pCharacter->m_Weapon] = 10;
    }
  } else {
    ResetWeapons();
  }

  RenderEdges();
  RenderTriangles();

  if (!m_aEntities.empty() && pCharacter) {
    RenderPath(FindPath(vec2(pCharacter->m_X, pCharacter->m_Y), m_Destination));
  }
}

void CBot::OnMessage(int MsgType, void * pRawMsg) {
	if(MsgType == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
    m_WeaponPickup = pMsg->m_Weapon + 1;
	}
}

void CBot::OnPlayerDeath() {
  ResetWeapons();
}

void CBot::OnStartGame() {
  ResetWeapons();
}

void CBot::ResetWeapons() {
  m_aWeapons[WEAPON_HAMMER] = -1;
  m_aWeapons[WEAPON_GUN] = 10;
  m_aWeapons[WEAPON_SHOTGUN] = 0;
  m_aWeapons[WEAPON_GRENADE] = 0;
  m_aWeapons[WEAPON_LASER] = 0;
  m_aWeapons[WEAPON_NINJA] = 0;

  m_WeaponPickup = 0;
}

void CBot::LoadMapData() {
	m_aEntities.clear();

	if (m_pField != nullptr) {
		delete[] m_pField;
    m_pField = nullptr;
  }
	if (m_pJPSField != nullptr) {
		delete[] m_pJPSField;
    m_pJPSField = nullptr;
  }

	CMapItemLayerTilemap *pTileMap = Layers()->GameLayer();
	CTile *pTiles = (CTile *)Kernel()->RequestInterface<IMap>()->GetData(pTileMap->m_Data);

	m_FieldWidth = pTileMap->m_Width;
	m_FieldHeight = pTileMap->m_Height;
	m_pField = new char[m_FieldWidth * m_FieldHeight];
  m_pJPSField = new char[m_FieldWidth * m_FieldHeight];

	for (int y = 0; y < pTileMap->m_Height; y++) {
		for (int x = 0; x < pTileMap->m_Width; x++) {
			int index = pTiles[y * pTileMap->m_Width + x].m_Reserved;

      m_pField[GetFieldIndex(x, y)] = 0;
      m_pJPSField[GetFieldIndex(x, y)] = 1;

      if (index == TILE_SOLID) {
        m_pField[GetFieldIndex(x, y)] = 1;
        m_pJPSField[GetFieldIndex(x, y)] = 0;
      } else if (index == TILE_NOHOOK) {
        m_pField[GetFieldIndex(x, y)] = 2;
        m_pJPSField[GetFieldIndex(x, y)] = 0;
      } else if (index >= ENTITY_OFFSET) {
				CBotEntity entity(ivec2(x, y), index - ENTITY_OFFSET);

        if (entity.m_Type == ENTITY_SPAWN || entity.m_Type == ENTITY_SPAWN_RED || entity.m_Type == ENTITY_SPAWN_BLUE)
          continue;

				m_aEntities.push_back(entity);
			}
		}
	}

  FindEdges();

  m_TrianglesLock.lock();
  if (LoadTriangles(false)) {
    m_bTrianglesReady.store(true);
    m_TriangulationID = 0;
  } else {
    m_TriangulationID = rand();
    new std::thread([this]{
      this->TriangulateMap(m_TriangulationID, m_aEdges);
    });
  }
  m_TrianglesLock.unlock();

  ChooseDestination();
}

CNetObj_PlayerInput CBot::GetInputData(CNetObj_PlayerInput lastData) {
	CNetObj_PlayerInput input = {0};

  m_PreviousInput = lastData;

  const CNetObj_Character * pPlayerChar = m_pClient->m_Snap.m_pLocalCharacter;
  if (pPlayerChar) {
    //auto startAt = time_get();

    ChooseDestination();

    input = Move();

    if (m_WeaponPickup && pPlayerChar->m_Weapon != m_WeaponPickup - 1) {
      input.m_WantedWeapon = m_WeaponPickup;
      m_WeaponPickup = 0;
    } else if (m_PreviousInput.m_Fire == 0 && (input.m_Hook == 0 || m_PreviousInput.m_Hook == 1)) {
      auto shootInput = Shoot(input);

      input.m_WantedWeapon = shootInput.m_WantedWeapon;
      input.m_TargetX = shootInput.m_TargetX;
      input.m_TargetY = shootInput.m_TargetY;
      input.m_Fire = shootInput.m_Fire;
    } else {
      input.m_WantedWeapon = 0;
      input.m_Fire = 0;
    }

    if (input.m_Fire == 0 && (input.m_Hook == 0 || m_PreviousInput.m_Hook == 1)) {
      input.m_TargetX = m_PreviousInput.m_TargetX;
      input.m_TargetY = m_PreviousInput.m_TargetY;
    }

    /*{
      char aBuf[256];
      str_format(
        aBuf, sizeof(aBuf),
        "dir %2d, jump %d, hook %d, fire %d, x=%d, y=%d (took %.2f s)",
        input.m_Direction,
        input.m_Jump,
        input.m_Hook,
        input.m_Fire,
        input.m_TargetX,
        input.m_TargetY,
        (double)(time_get() - startAt) / (double)time_freq()
      );
      Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "bot", aBuf);
    }*/
  }

  input.m_PlayerFlags |= PLAYERFLAG_BOT;

	return input;
}

bool CompareNeighbors(const char *neighbors, const char *mask) {
  for (int i = 0; i < 9; i++) {
    if (mask[i] == '?') {
      continue;
    }

    if (neighbors[i] != mask[i]) {
      return false;
    }
  }

  return true;
}

void CBot::FindEdges() {
  m_aEdges.clear();

  m_aEdges.push_back(vec2(0, 0));
  m_aEdges.push_back(vec2(m_FieldWidth * 32.0f, 0));
  m_aEdges.push_back(vec2(0, m_FieldHeight * 32.0f));
  m_aEdges.push_back(vec2(m_FieldWidth * 32.0f, m_FieldHeight * 32.0f));

  printf("\n");
  for (int y = 0; y < m_FieldHeight; y++) {
    for (int x = 0; x < m_FieldWidth; x++) {
      char neighbors[10] = {0};
      int i = 0;

      for (int qy = y - 1; qy <= y + 1; qy++) {
        for (int qx = x - 1; qx <= x + 1; qx++) {
          if (qx < 0 || qy < 0 || qx >= m_FieldWidth || qy >= m_FieldHeight) {
            neighbors[i++] = ' ';
          } else {
            neighbors[i++] = m_pField[GetFieldIndex(qx, qy)] == 0 ? ' ' : 'X';
          }
        }
      }

      bool isEdge = false;

      if (CompareNeighbors(neighbors, "XX?X ????")) {
        // XX?
        // X_?
        // ???

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f + 1.0f, y * 32.0f + 1.0f));
      }
      
      if (CompareNeighbors(neighbors, "?XX? X???")) {
        // ?XX
        // ?_X
        // ???

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f + 31.0f, y * 32.0f));
      }
      
      if (CompareNeighbors(neighbors, "???X ?XX?")) {
        // ???
        // X_?
        // XX?

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f + 1.0f, y * 32.0f + 31.0f));
      }
      
      if (CompareNeighbors(neighbors, "???? X?XX")) {
        // ???
        // ?_X
        // ?XX

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f + 31.0f, y * 32.0f + 31.0f));
      }
      
      if (CompareNeighbors(neighbors, "  ? XX?X?")) {
        // __?
        // _XX
        // ?X?

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f - 1.0f, y * 32.0f - 1.0f));
      }
      
      if (CompareNeighbors(neighbors, "?  XX ?X?")) {
        // ?__
        // XX_
        // ?X?

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f + 33.0f, y * 32.0f - 1.0f));
      }
      
      if (CompareNeighbors(neighbors, "?X?XX ?  ")) {
        // ?X?
        // XX_
        // ?__

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f + 33.0f, y * 32.0f + 33.0f));
      }
      
      if (CompareNeighbors(neighbors, "?X? XX  ?")) {
        // ?X?
        // _XX
        // __?

        isEdge = true;
        m_aEdges.push_back(vec2(x * 32.0f - 1.0f, y * 32.0f + 33.0f));
      }

      if (isEdge) {
        printf("X");
      } else if (m_pField[GetFieldIndex(x, y)]) {
        printf(".");
      } else {
        printf(" ");
      }
    }
    printf("\n");
  }

  m_aEdges.unique();
}

void CBot::RenderEdges() {
  CUIRect Screen;
	Graphics()->GetScreen(&Screen.x, &Screen.y, &Screen.w, &Screen.h);

	vec2 Center = m_pClient->m_pCamera->m_Center;

	CMapItemGroup * pGroup = Layers()->GameGroup();

	float Points[4];
	RenderTools()->MapScreenToWorld(Center.x, Center.y, pGroup->m_ParallaxX/100.0f, pGroup->m_ParallaxY/100.0f,
		pGroup->m_OffsetX, pGroup->m_OffsetY, Graphics()->ScreenAspect(), 1.0f, Points);
	Graphics()->MapScreen(Points[0], Points[1], Points[2], Points[3]);

	Graphics()->BlendNormal();
	Graphics()->TextureSet(gs_EmptyTexture);
	Graphics()->QuadsBegin();

  const float q = 0.3;
	Graphics()->SetColor(q, q, q, 0.7f);

  std::for_each(m_aEdges.cbegin(), m_aEdges.cend(), [&](const vec2 &edge){
		IGraphics::CQuadItem QuadItem(edge.x - 4.0f, edge.y - 4.0f, 8, 8);
		Graphics()->QuadsDrawTL(&QuadItem, 1);
  });

	Graphics()->QuadsEnd();
}

void CBot::ResetTriangles(std::list<CBotTriangle*> &triangles) {
  std::for_each(m_aTriangles.cbegin(), m_aTriangles.cend(), [](const CBotTriangle* pTriangle){
    delete pTriangle;
  });
  m_aTriangles.clear();
}

void CBot::TriangulateMap(int ID, std::list<vec2> edges) {
  std::list<CBotTriangle *> triangles;

  auto startAt = time_get();

  std::mutex lock;
  std::for_each(std::execution::par, edges.cbegin(), edges.cend(), [&](const vec2 edgeA){
    std::for_each(std::execution::par, edges.cbegin(), edges.cend(), [&](const vec2 edgeB){
      if (edgeA == edgeB || Collision()->IntersectLineFast(edgeA, edgeB)) {
        return;
      }

      std::for_each(std::execution::par, edges.cbegin(), edges.cend(), [&](const vec2 edgeC){
        if (edgeC == edgeA || edgeC == edgeB || Collision()->IntersectLineFast(edgeA, edgeC) || Collision()->IntersectLineFast(edgeB, edgeC)) {
          return;
        }

        CBotTriangle *pTriangle = new CBotTriangle(edgeA, edgeB, edgeC);

        if (pTriangle->m_CircumСenter.x < 0 || pTriangle->m_CircumСenter.x > m_FieldWidth * 32.0f ||
            pTriangle->m_CircumСenter.y < 0 || pTriangle->m_CircumСenter.y > m_FieldHeight * 32.0f)
        {
          delete pTriangle;
          return;
        }

        if (std::any_of(std::execution::par, edges.cbegin(), edges.cend(), [&](const vec2 edgeQ){
          if (edgeQ == edgeA || edgeQ == edgeB || edgeQ == edgeC) {
            return false;
          }

          if (Collision()->IntersectLineFast(edgeQ, edgeA) || Collision()->IntersectLineFast(edgeQ, edgeB) || Collision()->IntersectLineFast(edgeQ, edgeC)) {
            return false;
          }

          return distance(vec2(edgeQ.x, edgeQ.y), pTriangle->m_CircumСenter) < pTriangle->m_CircumRadius;
        })) {
          delete pTriangle;
          return;
        }

        lock.lock();
        if (std::any_of(std::execution::par, triangles.cbegin(), triangles.cend(), [pTriangle](const CBotTriangle* pOtherTriangle){
          return pTriangle->IsEqual(pOtherTriangle);
        })) {
          lock.unlock();
          delete pTriangle;
          return;
        }

        triangles.push_back(pTriangle);
        lock.unlock();
      });
    });
  });

  m_TrianglesLock.lock();

  if (m_TriangulationID.compare_exchange_strong(ID, 0)) {
    m_aTriangles = triangles;
    FindTrianglesNeighbors(false);

    SaveTriangles(false);

    m_bTrianglesReady.store(true);

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "triangulation complete, %ld triangles found (took %.2f s)", m_aTriangles.size(), (double)(time_get() - startAt) / (double)time_freq());
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "bot", aBuf);
  } else {
    ResetTriangles(triangles);
  }

  m_TrianglesLock.unlock();
}

void CBot::FindTrianglesNeighbors(bool useLock) {
  if (useLock) {
    m_TrianglesLock.lock();
  }

  std::for_each(std::execution::par, m_aTriangles.begin(), m_aTriangles.end(), [&](CBotTriangle* pTriangle){
    std::for_each(m_aTriangles.begin(), m_aTriangles.end(), [&](CBotTriangle* pOtherTriangle){
      if (pTriangle != pOtherTriangle) {
        if (pTriangle->IsNeighbor(pOtherTriangle)) {
          pTriangle->m_aNeighbors.push_back(pOtherTriangle);
        }

        /*if (Collision()->IntersectLineFast(pTriangle->m_Center, pOtherTriangle->m_Center)) {
          pTriangle->m_aVisible.push_back(pOtherTriangle);
        }*/
      }
    });
  });

  m_aTriangles.remove_if([](CBotTriangle *pTriangle){
    if (pTriangle->m_aNeighbors.empty()) {
      delete pTriangle;
      return true;
    }

    return false;
  });

  if (useLock) {
    m_TrianglesLock.unlock();
  }
}

void CBot::RenderTriangles() {
	CUIRect Screen;
	Graphics()->GetScreen(&Screen.x, &Screen.y, &Screen.w, &Screen.h);

	vec2 Center = m_pClient->m_pCamera->m_Center;

	CMapItemGroup * pGroup = Layers()->GameGroup();

	float Points[4];
	RenderTools()->MapScreenToWorld(Center.x, Center.y, pGroup->m_ParallaxX/100.0f, pGroup->m_ParallaxY/100.0f,
		pGroup->m_OffsetX, pGroup->m_OffsetY, Graphics()->ScreenAspect(), 1.0f, Points);
	Graphics()->MapScreen(Points[0], Points[1], Points[2], Points[3]);

	Graphics()->BlendNormal();
	Graphics()->TextureSet(gs_EmptyTexture);

	IGraphics::CLineItem Array[1024];
	int NumItems = 0;
	Graphics()->LinesBegin();
  Graphics()->SetColor(1, 1, 1, 1);

  m_TrianglesLock.lock();
  std::for_each(m_aTriangles.cbegin(), m_aTriangles.cend(), [&](const CBotTriangle* pTriangle){
    Array[NumItems++] = IGraphics::CLineItem(pTriangle->m_A.x, pTriangle->m_A.y, pTriangle->m_B.x, pTriangle->m_B.y);
    if(NumItems == 1024)
    {
      Graphics()->LinesDraw(Array, 1024);
      NumItems = 0;
    }

    Array[NumItems++] = IGraphics::CLineItem(pTriangle->m_C.x, pTriangle->m_C.y, pTriangle->m_B.x, pTriangle->m_B.y);
    if(NumItems == 1024)
    {
      Graphics()->LinesDraw(Array, 1024);
      NumItems = 0;
    }

    Array[NumItems++] = IGraphics::CLineItem(pTriangle->m_A.x, pTriangle->m_A.y, pTriangle->m_C.x, pTriangle->m_C.y);
    if(NumItems == 1024)
    {
      Graphics()->LinesDraw(Array, 1024);
      NumItems = 0;
    }
  });
  m_TrianglesLock.unlock();

	if(NumItems)
		Graphics()->LinesDraw(Array, NumItems);
	Graphics()->LinesEnd();
}

const CBotTriangle * CBot::FindTriangle(vec2 point, bool useLock) {
  if (useLock) {
    m_TrianglesLock.lock();
  }

  auto result = std::find_if(m_aTriangles.begin(), m_aTriangles.end(), [point](CBotTriangle *pTriangle) {
    return pTriangle->IsInside(point);
  });

  if (result == m_aTriangles.end()) {
    if (useLock) {
      m_TrianglesLock.unlock();
    }

    return nullptr;
  }

  if (useLock) {
    m_TrianglesLock.unlock();
  }

  return *result;
}

const CBotTriangle * CBot::FindNearestTriangle(vec2 point, bool useLock) {
  if (useLock) {
    m_TrianglesLock.lock();
  }

  auto result = std::min_element(m_aTriangles.cbegin(), m_aTriangles.cend(), [point](const CBotTriangle* lhs, const CBotTriangle* rhs) {
    return distance(lhs->m_Center, point) < distance(rhs->m_Center, point);
  });

  if (result == m_aTriangles.end()) {
    if (useLock) {
      m_TrianglesLock.unlock();
    }

    return nullptr;
  }

  if (useLock) {
    m_TrianglesLock.unlock();
  }

  return *result;
}

const CBotTriangle * CBot::FindNearestVisibleTriangle(vec2 point, bool useLock) {
  if (useLock) {
    m_TrianglesLock.lock();
  }

  std::list<const CBotTriangle *> visible;
  std::for_each(m_aTriangles.begin(), m_aTriangles.end(), [&](const CBotTriangle *pTriangle){
    if (!Collision()->IntersectLineFast(point, pTriangle->m_Center)) {
      visible.push_back(pTriangle);
    }
  });

  auto result = std::min_element(visible.cbegin(), visible.cend(), [point](const CBotTriangle* lhs, const CBotTriangle* rhs) {
    return distance(lhs->m_Center, point) < distance(rhs->m_Center, point);
  });

  if (result == visible.end()) {
    if (useLock) {
      m_TrianglesLock.unlock();
    }

    return nullptr;
  }

  if (useLock) {
    m_TrianglesLock.unlock();
  }

  return *result;
}

void CBot::SaveTriangles(bool useLock) {
  if (useLock) {
    m_TrianglesLock.lock();
  }

  CServerInfo currentServerInfo;
	Client()->GetServerInfo(&currentServerInfo);

	char filename[512] = {0};
	str_format(filename, sizeof(filename), "bot/%s.dat", currentServerInfo.m_aMap);
  IOHANDLE file = Storage()->OpenFile(filename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
  if (!file) {
    if (useLock) {
      m_TrianglesLock.unlock();
    }

    return;
  }

  std::size_t edgesCount = m_aEdges.size();
  io_write(file, &edgesCount, sizeof(edgesCount));

  std::for_each(m_aEdges.cbegin(), m_aEdges.cend(), [&](const vec2 &edge) {
    io_write(file, &edge.x, sizeof(edge.x));
    io_write(file, &edge.y, sizeof(edge.y));
  });

  std::size_t trianglesCount = m_aTriangles.size();
  io_write(file, &trianglesCount, sizeof(trianglesCount));

  std::for_each(m_aTriangles.cbegin(), m_aTriangles.cend(), [&](const CBotTriangle *pTriangle) {
    io_write(file, &pTriangle->m_A.x, sizeof(pTriangle->m_A.x));
    io_write(file, &pTriangle->m_A.y, sizeof(pTriangle->m_A.y));

    io_write(file, &pTriangle->m_B.x, sizeof(pTriangle->m_B.x));
    io_write(file, &pTriangle->m_B.y, sizeof(pTriangle->m_B.y));

    io_write(file, &pTriangle->m_C.x, sizeof(pTriangle->m_C.x));
    io_write(file, &pTriangle->m_C.y, sizeof(pTriangle->m_C.y));
  });

  io_close(file);

  if (useLock) {
    m_TrianglesLock.unlock();
  }
}

bool CBot::LoadTriangles(bool useLock) {
  if (useLock) {
    m_TrianglesLock.lock();
  }

  CServerInfo currentServerInfo;
	Client()->GetServerInfo(&currentServerInfo);

  char filename[512] = {0};
	str_format(filename, sizeof(filename), "bot/%s.dat", currentServerInfo.m_aMap);
  IOHANDLE file = Storage()->OpenFile(filename, IOFLAG_READ, IStorage::TYPE_SAVE);
  if (!file) {
    if (useLock) {
      m_TrianglesLock.unlock();
    }
    
    return false;
  }

  m_aEdges.clear();
  ResetTriangles(m_aTriangles);

  std::size_t edgesCount;
  io_read(file, &edgesCount, sizeof(edgesCount));

  for (std::size_t i = 0; i < edgesCount; i++) {
    vec2 edge;
    io_read(file, &edge.x, sizeof(edge.x));
    io_read(file, &edge.y, sizeof(edge.y));

    m_aEdges.push_back(edge);
  }

  std::size_t trianglesCount;
  io_read(file, &trianglesCount, sizeof(trianglesCount));

  for (std::size_t i = 0; i < trianglesCount; i++) {
    vec2 a, b, c;

    io_read(file, &a.x, sizeof(a.x));
    io_read(file, &a.y, sizeof(a.y));

    io_read(file, &b.x, sizeof(b.x));
    io_read(file, &b.y, sizeof(b.y));

    io_read(file, &c.x, sizeof(c.x));
    io_read(file, &c.y, sizeof(c.y));

    m_aTriangles.push_back(new CBotTriangle(a, b, c));
  }

  FindTrianglesNeighbors(false);

  if (useLock) {
    m_TrianglesLock.unlock();
  }

  return true;
}

std::list<vec2> CBot::FindPath(vec2 from, vec2 to, bool useDirect) {
  std::list<vec2> result;

  if (useDirect && !Collision()->IntersectLineFast(from, to)) {
    result.push_back(from);
    result.push_back(to);
    
    return result;
  }

  if (!m_bTrianglesReady.load()) {
    result.push_back(from);
    return result;
  }

  /*const auto filterResult = [&](){
    result.remove_if([&](vec2 point){
      return !Collision()->IntersectLineFast(from, point) || !Collision()->IntersectLineFast(point, to);
    });
  };*/

  m_TrianglesLock.lock();

  const CBotTriangle *pTriangleFrom = FindTriangle(from, false);
  const CBotTriangle *pTriangleTo = FindTriangle(to, false);

  if (!pTriangleFrom) {
    pTriangleFrom = FindNearestTriangle(from, false);
  }
  /*if (!pTriangleTo) {
    pTriangleTo = FindNearestTriangle(to, false);
  }*/

  if (!pTriangleFrom || !pTriangleTo) {
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "unable to find path from (x=%.0f, y=%.0f) to (x=%.0f, y=%.0f)", from.x, from.y, to.x, to.y);
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "bot", aBuf);

    // if we can't find suitable path, let's do this using A*-JPS library

    int indexFrom = GetFieldIndex(
      clamp((int)round(from.x / 32.0f), 0, m_FieldWidth - 1),
      clamp((int)round(from.y / 32.0f), 0, m_FieldHeight - 1)
    );
    int indexTo = GetFieldIndex(
      clamp((int)round(to.x / 32.0f), 0, m_FieldWidth - 1),
      clamp((int)round(to.y / 32.0f), 0, m_FieldHeight - 1)
    );

    auto itCached = m_JPSCache.find(std::make_pair(indexFrom, indexTo));
    if (itCached != m_JPSCache.end()) {
      result = itCached->second;

      result.push_front(from);
      result.push_back(to);

      m_TrianglesLock.unlock();
      return result;
    }

    int solutionLength;
    int *paSolution = astar_compute((const char *)m_pJPSField, &solutionLength, m_FieldWidth, m_FieldHeight, indexFrom, indexTo);
    if (paSolution) {
      result.push_back(from);
      for (int i = 0; i < solutionLength; i++) {
        ivec2 fieldPos = GetPositionFromIndex(paSolution[i]);
        vec2 pos(
          fieldPos.x * 32.0f + 16.0f,
          fieldPos.y * 32.0f + 16.0f
        );

        result.push_back(pos);
      }
      result.push_back(to);

      free(paSolution);

      m_JPSCache[std::make_pair(indexFrom, indexTo)] = result;

      m_TrianglesLock.unlock();
      return result;
    }
    
    m_TrianglesLock.unlock();
    result.push_back(from);
    return result;
  }

  auto itCached = m_PathCache.find(std::make_pair(pTriangleFrom, pTriangleTo));
  if (itCached != m_PathCache.end()) {
    result = itCached->second;
    //filterResult();

    result.push_front(from);
    result.push_back(to);

    m_TrianglesLock.unlock();
    return result;
  }

  std::map<const CBotTriangle *, const CBotTriangle *> cameFrom;
  std::map<const CBotTriangle *, double> gScore;
  std::map<const CBotTriangle *, double> fScore;

  std::set<const CBotTriangle*> openSet;
  openSet.insert(pTriangleFrom);

  while (!openSet.empty()) {
    auto itCurrent = std::min_element(openSet.begin(), openSet.end(), [&fScore](const CBotTriangle* lhs, const CBotTriangle* rhs) {
      return fScore[lhs] < fScore[rhs];
    });

    const CBotTriangle *pCurrent = *itCurrent;
    if (pCurrent == pTriangleTo) {
      break;
    }

    openSet.erase(itCurrent);

    std::for_each(pCurrent->m_aNeighbors.begin(), pCurrent->m_aNeighbors.end(), [&](const CBotTriangle *pNeighbor){
      if (Collision()->IntersectLine(pCurrent->m_Center, pNeighbor->m_Center, nullptr, nullptr)) {
        return;
      }

      double score = distance(pCurrent->m_Center, pNeighbor->m_Center);

      double tentativeGScore = gScore[pCurrent] + score;
      auto neighborGScore = gScore.try_emplace(pNeighbor, INFINITY);
      if (tentativeGScore < neighborGScore.first->second) {
        cameFrom[pNeighbor] = pCurrent;

        gScore[pNeighbor] = tentativeGScore;
        fScore[pNeighbor] = tentativeGScore;

        openSet.insert(pNeighbor);
      }
    });
  }

  const CBotTriangle *pTriangle = pTriangleTo;
  while (pTriangle) {
    result.push_front(pTriangle->m_Center);
    pTriangle = cameFrom[pTriangle];
  }

  m_PathCache[std::make_pair(pTriangleFrom, pTriangleTo)] = result;

  //filterResult();

  result.push_front(from);
  result.push_back(to);

  m_TrianglesLock.unlock();

  return result;
}

void CBot::RenderPath(std::list<vec2> path) {
  if (path.empty()) {
    return;
  }

  CUIRect Screen;
	Graphics()->GetScreen(&Screen.x, &Screen.y, &Screen.w, &Screen.h);

	vec2 Center = m_pClient->m_pCamera->m_Center;

	CMapItemGroup * pGroup = Layers()->GameGroup();

	float Points[4];
	RenderTools()->MapScreenToWorld(Center.x, Center.y, pGroup->m_ParallaxX/100.0f, pGroup->m_ParallaxY/100.0f,
		pGroup->m_OffsetX, pGroup->m_OffsetY, Graphics()->ScreenAspect(), 1.0f, Points);
	Graphics()->MapScreen(Points[0], Points[1], Points[2], Points[3]);

	Graphics()->BlendNormal();
	Graphics()->TextureSet(gs_EmptyTexture);

	IGraphics::CLineItem Array[1024];
	int NumItems = 0;
	Graphics()->LinesBegin();
  Graphics()->SetColor(1, 0, 0, 1);

  vec2 previous = *path.cbegin();

  std::for_each(path.cbegin(), path.cend(), [&](const vec2 point){
    Array[NumItems++] = IGraphics::CLineItem(previous.x, previous.y, point.x, point.y);
    if(NumItems == 1024)
    {
      Graphics()->LinesDraw(Array, 1024);
      NumItems = 0;
    }

    previous = point;
  });

	if(NumItems)
		Graphics()->LinesDraw(Array, NumItems);
	Graphics()->LinesEnd();
}

float CBot::GetPathDistance(std::list<vec2> path) {
  if (path.size() < 2) {
    return -1;
  }

  float dist = 0.0f;

  auto previous = path.cbegin();
  for (auto current = std::next(path.cbegin()); current != path.cend(); ++current) {
    dist += distance(*previous, *current);
    previous = current;
  }

  return dist;
}

float CBot::GetPathDistance(vec2 from, vec2 to) {
  return GetPathDistance(FindPath(from, to));
}

int CBot::GetPathWaypointsCount(std::list<vec2> path) {
  return path.size();
}

int CBot::GetPathWaypointsCount(vec2 from, vec2 to) {
  return GetPathWaypointsCount(FindPath(from, to));
}

bool CBot::ValidatePath(std::list<vec2> path) {
  if (path.size() < 2) {
    return false;
  }

  vec2 previous = *path.begin();
  for (auto it = std::next(path.begin()); it != path.end(); ++it) {
    if (Collision()->IntersectLineFast(previous, *it)) {
      return false;
    }

    previous = *it;
  }

  return true;
}

struct CBotMoveAttempt {
  public:
    int m_Iteration;
    CNetObj_PlayerInput m_InitialInput;
    float m_PathDistance;

    CNetObj_Character m_Character;
    CNetObj_PlayerInput m_Input;
    CCollision *m_pCollision;
    CTuningParams m_Tuning;

    CBotMoveAttempt(int iteration, CNetObj_Character character, CNetObj_PlayerInput input, CCollision *pCollision, CTuningParams tuning, CNetObj_PlayerInput initialInput) {
      m_Iteration = iteration;
      m_Character = character;
      m_Input = input;
      m_InitialInput = initialInput;
      m_Tuning = tuning;
      m_pCollision = pCollision;
    }

    void Predict(const int ticksCount) {
      CWorldCore tempWorld;
      tempWorld.m_Tuning = m_Tuning;

      CCharacterCore tempCore;
      mem_zero(&tempCore, sizeof(tempCore));
      tempCore.Init(&tempWorld, m_pCollision);
      tempCore.Read(&m_Character);
      tempCore.m_Input = m_Input;

      for (int i = 0; i < ticksCount; i++) {
        tempCore.Tick(true);
        tempCore.Move();
        tempCore.Quantize();
      }

      tempCore.Write(&m_Character);
    }
};

CNetObj_PlayerInput CBot::Move(const int iterationsCount, const int predictionTicks) {
  if (!m_bTrianglesReady.load()) {
    return CNetObj_PlayerInput();
  }

  //auto startAt = time_get();

  CNetObj_Character currentCharacter = m_pClient->m_Snap.m_aCharacters[m_pClient->m_LocalClientID].m_Cur;

  std::list<CBotMoveAttempt> currentIteraction;
  std::list<CBotMoveAttempt> nextIteraction;

  CBotMoveAttempt initial(
    -1,
    currentCharacter,
    m_PreviousInput,
    Collision(),
    m_pClient->m_Tuning,
    m_PreviousInput
  );
  initial.m_PathDistance = GetPathDistance(vec2(currentCharacter.m_X, currentCharacter.m_Y), m_Destination);
  currentIteraction.push_back(initial);

  if (initial.m_PathDistance < 0) {
    ChooseDestination();
    return m_PreviousInput;
  }

  for (int iteration = 0; iteration < iterationsCount; iteration++) {
    std::for_each(std::execution::par, currentIteraction.cbegin(), currentIteraction.cend(), [&](const CBotMoveAttempt &current){
      for (int direction = -1; direction <= 1; direction++) {
        for (int jump = 0; jump <= 1; jump++) {
          for (int hook = 0; hook <= (iteration == 0 ? 5 : 1); hook++) {
            CNetObj_PlayerInput input = current.m_Input;
            input.m_Direction = direction;
            input.m_Jump = jump;

            if (hook == 0) {
              input.m_Hook = 0;
            } else if (hook == 1) {
              input.m_Hook = 1;
            } else {
              const CBotTriangle *pTriangle = FindTriangle(vec2(current.m_Character.m_X, current.m_Character.m_Y));

              if (!pTriangle) return;
              
              input.m_Hook = 1;
              
              if (hook == 2) {
                input.m_TargetX = pTriangle->m_A.x - current.m_Character.m_X;
                input.m_TargetY = pTriangle->m_A.y - current.m_Character.m_Y;
              } else if (hook == 3) {
                input.m_TargetX = pTriangle->m_B.x - current.m_Character.m_X;
                input.m_TargetY = pTriangle->m_B.y - current.m_Character.m_Y;
              } else if (hook == 4) {
                input.m_TargetX = pTriangle->m_C.x - current.m_Character.m_X;
                input.m_TargetY = pTriangle->m_C.y - current.m_Character.m_Y;
              } else {
                input.m_TargetX = (rand()%100 - rand()%100);
                input.m_TargetY = (rand()%100 - rand()%100);
              }
            }

            nextIteraction.push_back(CBotMoveAttempt(
              iteration,
              current.m_Character,
              input,
              Collision(),
              m_pClient->m_Tuning,
              iteration == 0 ? input : current.m_InitialInput
            ));
          }
        }
      }
    });

    std::for_each(std::execution::par, nextIteraction.begin(), nextIteraction.end(), [predictionTicks](CBotMoveAttempt &attempt){
      attempt.Predict(predictionTicks);
    });

    std::for_each(std::execution::par, nextIteraction.begin(), nextIteraction.end(), [&](CBotMoveAttempt &attempt){
      attempt.m_PathDistance = GetPathDistance(vec2(attempt.m_Character.m_X, attempt.m_Character.m_Y), m_Destination);
    });

    nextIteraction.remove_if([&](CBotMoveAttempt &attempt){
      return attempt.m_PathDistance < 0;
    });

    currentIteraction.clear();
    std::move(nextIteraction.begin(), nextIteraction.end(), std::back_inserter(currentIteraction));
  }

  auto itBest = std::min_element(currentIteraction.begin(), currentIteraction.end(), [](const CBotMoveAttempt lhs, const CBotMoveAttempt rhs) {
    return lhs.m_PathDistance < rhs.m_PathDistance;
  });

  // if bot stuck, let him choose worst movement - maybe this can make bot move
  if (itBest->m_Character.m_X == currentCharacter.m_X && itBest->m_Character.m_Y == currentCharacter.m_Y) {
    itBest = std::max_element(currentIteraction.begin(), currentIteraction.end(), [](const CBotMoveAttempt lhs, const CBotMoveAttempt rhs) {
      return lhs.m_PathDistance < rhs.m_PathDistance;
    });
  }

  /*{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "next move found: direction %d, jump %d (took %.2f s)", itBest->m_InitialInput.m_Direction, itBest->m_InitialInput.m_Jump, (double)(time_get() - startAt) / (double)time_freq());
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "bot", aBuf);
  }*/

  return itBest->m_InitialInput;
}

bool CBot::IsTeamplay() {
  if (m_pClient->m_Snap.m_pGameData) {
		return (m_pClient->m_GameInfo.m_GameFlags&GAMEFLAG_TEAMS) != 0;
  }

  return false;
}

bool CBot::IsCTF() {
  if (m_pClient->m_Snap.m_pGameData) {
		return (m_pClient->m_GameInfo.m_GameFlags&GAMEFLAG_FLAGS) != 0;
  }

  return false;
}

bool CBot::IsEnemy(int clientID) {
  if (clientID == m_pClient->m_LocalClientID || !m_pClient->m_Snap.m_aCharacters[clientID].m_Active) {
    return false;
  }

  if (!m_pClient->m_Snap.m_pGameData || !m_pClient->m_Snap.m_pLocalInfo || !m_pClient->m_Snap.m_paPlayerInfos[clientID]) {
    return false;
  }

  if (m_pClient->m_Snap.m_paPlayerInfos[clientID]->m_PlayerFlags&PLAYERFLAG_CHATTING) {
    return false;
  }

  if (!IsTeamplay()) {
    return true;
  }

  return m_pClient->m_aClients[m_pClient->m_LocalClientID].m_Team != m_pClient->m_aClients[clientID].m_Team;
}

std::list<CNetObj_Character> CBot::GetEnemies() {
  std::list<CNetObj_Character> enemies;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (IsEnemy(i)) {
      enemies.push_back(m_pClient->m_Snap.m_aCharacters[i].m_Cur);
      break;
    }
  }

  return enemies;
}

const std::tuple<float, float, float> CBot::GetWeaponTuning(int weapon) {
  switch (weapon) {
  case WEAPON_HAMMER:
    return std::make_tuple(0.0f, 100.0f, Client()->GameTickSpeed());

  case WEAPON_GUN:
    return std::make_tuple(m_pClient->m_Tuning.m_GunCurvature, m_pClient->m_Tuning.m_GunSpeed, m_pClient->m_Tuning.m_GunLifetime);

  case WEAPON_SHOTGUN:
    return std::make_tuple(m_pClient->m_Tuning.m_ShotgunCurvature, m_pClient->m_Tuning.m_ShotgunSpeed, m_pClient->m_Tuning.m_ShotgunLifetime);

  case WEAPON_GRENADE:
    return std::make_tuple(m_pClient->m_Tuning.m_GrenadeCurvature, m_pClient->m_Tuning.m_GrenadeSpeed, m_pClient->m_Tuning.m_GrenadeLifetime);

  case WEAPON_LASER:
    return std::make_tuple(0.0f, 7000.0f, Client()->GameTickSpeed());

  case WEAPON_NINJA:
    return std::make_tuple(0.0f, 800.0f, Client()->GameTickSpeed());

  default:
    return std::make_tuple(0.0f, 0.0f, 0.0f);
  }
}

CNetObj_PlayerInput CBot::Shoot(CNetObj_PlayerInput currentInput) {
  CNetObj_PlayerInput input = currentInput;

  input.m_Fire = 0;

  CNetObj_Character currentCharacter = m_pClient->m_Snap.m_aCharacters[m_pClient->m_LocalClientID].m_Cur;
  CBotMoveAttempt nextMove(
    0,
    currentCharacter,
    input,
    Collision(),
    m_pClient->m_Tuning,
    input
  );
  nextMove.Predict(Client()->PredGameTick() - Client()->GameTick());

  vec2 localPosition(nextMove.m_Character.m_X, nextMove.m_Character.m_Y);

  std::list<CNetObj_Character> enemies = GetEnemies();

  if (enemies.empty()) {
    return input;
  }

  std::list<CBotMoveAttempt> enemyMoves;
  std::for_each(enemies.cbegin(), enemies.cend(), [&](const CNetObj_Character enemy){
    CNetObj_PlayerInput enemyInput = {0};
    enemyInput.m_Direction = enemy.m_Direction;
    enemyInput.m_Hook = enemy.m_HookState != HOOK_IDLE;
    enemyInput.m_TargetX = enemy.m_HookX;
    enemyInput.m_TargetY = enemy.m_HookY;

    CBotMoveAttempt enemyMove(
      0,
      enemy,
      enemyInput,
      Collision(),
      m_pClient->m_Tuning,
      enemyInput
    );
    enemyMove.Predict(Client()->PredGameTick() - Client()->GameTick());

    enemyMoves.push_back(enemyMove);
  });

  auto intersectCharacter = [&](CNetObj_Character character, vec2 pos0, vec2 pos1, float radius){
    vec2 characterPos(character.m_X, character.m_Y);
    vec2 intersectPos = closest_point_on_line(pos0, pos1, characterPos);
    float len = distance(characterPos, intersectPos);
    return len < 28.0f + radius;
  };

  bool availableWeapons[NUM_WEAPONS] = {false};
  if (currentCharacter.m_Weapon == WEAPON_NINJA) {
    availableWeapons[WEAPON_NINJA] = true;
  } else if (m_PreviousInput.m_NextWeapon != 0) {
    availableWeapons[m_PreviousInput.m_NextWeapon - 1] = true;
  } else {
    for (int i = 0; i < NUM_WEAPONS; i++) {
      availableWeapons[i] = m_aWeapons[i] != 0;
    }

    availableWeapons[currentCharacter.m_Weapon] = currentCharacter.m_AmmoCount != 0;

    availableWeapons[WEAPON_HAMMER] = false; // hammerfight is not implemented
  }

  double maxLifetime = 0.0;
  for (int i = 0; i < NUM_WEAPONS; i++) {
    if (!availableWeapons[i]) {
      continue;
    }

    double curvature, speed, lifetime;
    std::tie(curvature, speed, lifetime) = GetWeaponTuning(i);

    if (lifetime > maxLifetime) {
      maxLifetime = lifetime;
    }
  }

  bool hasCollision[NUM_WEAPONS][360] = {0};
  bool inputFound[NUM_WEAPONS] = {false};
  CNetObj_PlayerInput inputs[NUM_WEAPONS] = {0};
  int bestWeapon = -1;
  int bestNonGunWeapon = -1;

  const float quant = 1.0f / Client()->GameTickSpeed();
  for (float time = quant; time <= maxLifetime; time += quant) {
    std::for_each(std::execution::par, enemyMoves.begin(), enemyMoves.end(), [&](CBotMoveAttempt &enemyMove){
      enemyMove.Predict(1);
    });

    for (int weapon = NUM_WEAPONS - 1; weapon >= 0; weapon--) {
      if (!availableWeapons[weapon] || inputFound[weapon]) {
        continue;
      }

      double curvature, speed, lifetime;
      std::tie(curvature, speed, lifetime) = GetWeaponTuning(weapon);

      for (int a = 0; a < 360; a += 2) {
        if (hasCollision[weapon][a]) {
          continue;
        }

        float angle = a * (M_PI / 180.0f);
        vec2 direction = normalize(vec2(cosf(angle) - sinf(angle), sinf(angle) + cosf(angle)));

        vec2 localGunPosition = localPosition + direction * 24.0f * 0.75f;

        vec2 projectilePrevPos = CalcPos(localGunPosition, direction, curvature, speed, time - quant);
        vec2 projectilePos = CalcPos(localGunPosition, direction, curvature, speed, time);

        hasCollision[weapon][a] = Collision()->IntersectLine(projectilePrevPos, projectilePos, &projectilePos, NULL);

        if (std::any_of(enemyMoves.cbegin(), enemyMoves.cend(), [&](const CBotMoveAttempt &enemyMove){
          return intersectCharacter(enemyMove.m_Character, projectilePrevPos, projectilePos, 6.0f);
        })) {
          if (currentCharacter.m_Weapon != weapon) {
            input.m_WantedWeapon = weapon + 1;
          } else {
            input.m_Fire = 1;
            input.m_TargetX = direction.x * 100.0f;
            input.m_TargetY = direction.y * 100.0f;
          }

          inputFound[weapon] = true;
          inputs[weapon] = input;

          if (bestWeapon < 0) {
            bestWeapon = weapon;
          }

          if (weapon != WEAPON_GUN && bestNonGunWeapon < 0) {
            bestNonGunWeapon = weapon;
          }
        }
      }
    }
  }

  /*if (inputFound[currentCharacter.m_Weapon] && currentCharacter.m_AmmoCount != 0) {
    input = inputs[currentCharacter.m_Weapon];
  } else */if (bestNonGunWeapon >= 0) {
    input = inputs[bestNonGunWeapon];
  } else if (bestWeapon >= 0) {
    input = inputs[bestWeapon];
  }

  return input;
}

std::list<CBotEntity> CBot::GetActiveEntities() {
  std::list<CBotEntity> entities;

  bool foundRedFlag = false;
  bool foundBlueFlag = false;

  int count = Client()->SnapNumItems(IClient::SNAP_CURRENT);
  for (int i = 0; i < count; i++) {
    IClient::CSnapItem item;
    const void *pData = Client()->SnapGetItem(IClient::SNAP_CURRENT, i, &item);

    int entityType;

    if (item.m_Type == NETOBJTYPE_PICKUP) {
      const CNetObj_Pickup * pPickup = static_cast<const CNetObj_Pickup *>(pData);
      ivec2 os((int)round(pPickup->m_X / 32.0f), (int)round(pPickup->m_Y / 32.0f));

      switch (pPickup->m_Type) {
      case PICKUP_HEALTH:
        entityType = ENTITY_HEALTH_1;
        break;
      case PICKUP_ARMOR:
        entityType = ENTITY_ARMOR_1;
        break;
      case PICKUP_GRENADE:
        entityType = ENTITY_WEAPON_GRENADE;
        break;
      case PICKUP_SHOTGUN:
        entityType = ENTITY_WEAPON_SHOTGUN;
        break;
      case PICKUP_LASER:
        entityType = ENTITY_WEAPON_LASER;
        break;
      case PICKUP_NINJA:
        entityType = ENTITY_POWERUP_NINJA;
        break;
      default:
        continue;
      }

      entities.push_back(CBotEntity(vec2(pPickup->m_X, pPickup->m_Y), entityType));
    } else if (item.m_Type == NETOBJTYPE_FLAG) {
      const CNetObj_Flag *pFlag = static_cast<const CNetObj_Flag *>(pData);

      switch (pFlag->m_Team) {
      case TEAM_RED:
        foundRedFlag = true;

        if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierRed == FLAG_ATSTAND) {
          entityType = ENTITY_FLAGSTAND_RED;
        } else {
          entityType = ENTITY_FLAG_CARRIED_RED;
        }
        break;

      case TEAM_BLUE:
        foundBlueFlag = true;

        if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierBlue == FLAG_ATSTAND) {
          entityType = ENTITY_FLAGSTAND_BLUE;
        } else {
          entityType = ENTITY_FLAG_CARRIED_BLUE;
        }
        break;
      }

      entities.push_back(CBotEntity(vec2(pFlag->m_X, pFlag->m_Y), entityType));
    }
  }

  if (IsTeamplay()) {
    if (!foundRedFlag) {
      auto entityIt = std::find_if(m_aEntities.begin(), m_aEntities.end(), [](CBotEntity &entity) {
        return entity.m_Type == ENTITY_FLAGSTAND_RED;
      });

      if (entityIt != m_aEntities.end()) {
        entities.push_back(*entityIt);
      }
    }

    if (!foundBlueFlag) {
      auto entityIt = std::find_if(m_aEntities.begin(), m_aEntities.end(), [](CBotEntity &entity) {
        return entity.m_Type == ENTITY_FLAGSTAND_BLUE;
      });

      if (entityIt != m_aEntities.end()) {
        entities.push_back(*entityIt);
      }
    }
  }

  auto enemies = GetEnemies();
  std::for_each(enemies.cbegin(), enemies.cend(), [&](const CNetObj_Character &enemy) {
    entities.push_back(CBotEntity(vec2(enemy.m_X, enemy.m_Y), ENTITY_ENEMY));
  });

  return entities;
}

struct CBotDestination {
  public:
    CBotEntity m_Entity;
    double m_Distance;
    int m_WaypointsCount;
    double m_Score;

    CBotDestination(CBotEntity entity, double distance, int waypointsCount, double score) {
      m_Entity = entity;
      m_Distance = distance;
      m_WaypointsCount = waypointsCount;
      m_Score = score;
    }

    double GetWeight() {
      return m_Score * m_Score - m_WaypointsCount * m_WaypointsCount;
    }
};

void CBot::ChooseDestination() {
  const CNetObj_Character *pPlayerChar = m_pClient->m_Snap.m_pLocalCharacter;
  if (!pPlayerChar || m_aEntities.empty()) {
    return;
  }

  auto playerInfo = m_pClient->m_aClients[m_pClient->m_LocalClientID];

  std::list<CBotEntity> entities = GetActiveEntities();

  if (entities.empty()) {
    m_Destination = std::next(m_aEntities.begin(), time_timestamp() % m_aEntities.size())->m_Position;
    return;
  }

  vec2 localPosition(pPlayerChar->m_X, pPlayerChar->m_Y);

  std::list<CBotDestination> destinations;
  std::for_each(entities.cbegin(), entities.cend(), [&](const CBotEntity &entity) {
    double score = 0;

    switch (entity.m_Type) {
    case ENTITY_HEALTH_1:
      if (pPlayerChar->m_Health == 10) {
        return;
      }

      score = (10 - pPlayerChar->m_Health) * (10 - pPlayerChar->m_Health);
      break;

    case ENTITY_ARMOR_1:
      if (pPlayerChar->m_Armor == 10) {
        return;
      }
      
      score = 10 - pPlayerChar->m_Armor;
      break;

    case ENTITY_WEAPON_GRENADE:
      if (m_aWeapons[WEAPON_GRENADE] == 10) {
        return;
      }

      score = 1 + (10 - m_aWeapons[WEAPON_GRENADE]);
      break;

    case ENTITY_WEAPON_SHOTGUN:
      if (m_aWeapons[WEAPON_SHOTGUN] == 10) {
        return;
      }
      
      score = 1 + (10 - m_aWeapons[WEAPON_SHOTGUN]);
      break;

    case ENTITY_WEAPON_LASER:
      if (m_aWeapons[WEAPON_LASER] == 10) {
        return;
      }
      
      score = 1 + (10 - m_aWeapons[WEAPON_LASER]);
      break;

    case ENTITY_POWERUP_NINJA:
      score = 8;
      break;

    case ENTITY_FLAGSTAND_RED:
      if (playerInfo.m_Team == TEAM_RED) {
        if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierBlue == m_pClient->m_LocalClientID) {
          score = 100;
        } else {
          return;
        }
      }

      score = 5;
      break;

    case ENTITY_FLAGSTAND_BLUE:
      if (playerInfo.m_Team == TEAM_BLUE) {
        if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierRed == m_pClient->m_LocalClientID) {
          score = 100;
        } else {
          return;
        }
      }
      
      score = 5;
      break;

    case ENTITY_FLAG_CARRIED_RED:
      if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierRed == m_pClient->m_LocalClientID) {
        return;
      }

      if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierRed >= 0 && playerInfo.m_Team == TEAM_BLUE) {
        return;
      }

      score = 20;
      break;

    case ENTITY_FLAG_CARRIED_BLUE:
      if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierBlue == m_pClient->m_LocalClientID) {
        return;
      }

      if (m_pClient->m_Snap.m_pGameDataFlag->m_FlagCarrierBlue >= 0 && playerInfo.m_Team == TEAM_RED) {
        return;
      }

      score = 20;
      break;
    }

    std::list<vec2> path = FindPath(localPosition, entity.m_Position, false);
    if (!ValidatePath(path)) {
      return;
    }

    destinations.push_back(CBotDestination(entity, GetPathDistance(path), GetPathWaypointsCount(path), score));
  });

  auto itDestination = std::max_element(destinations.begin(), destinations.end(), [&](CBotDestination &lhs, CBotDestination &rhs) {
    return lhs.GetWeight() < rhs.GetWeight();
  });

  if (itDestination == destinations.end()) {
    m_Destination = std::next(m_aEntities.begin(), time_timestamp() % m_aEntities.size())->m_Position;
    return;
  }

  float dist = distance(vec2(pPlayerChar->m_X, pPlayerChar->m_Y), itDestination->m_Entity.m_Position);
  if (dist < 32.0f) {
    if (itDestination->m_Entity.m_Type == ENTITY_WEAPON_SHOTGUN) {
      m_aWeapons[WEAPON_SHOTGUN] = 10;
    } else if (itDestination->m_Entity.m_Type == ENTITY_WEAPON_GRENADE) {
      m_aWeapons[WEAPON_GRENADE] = 10;
    } else if (itDestination->m_Entity.m_Type == ENTITY_WEAPON_LASER) {
      m_aWeapons[WEAPON_LASER] = 10;
    }
  }

  m_Destination = itDestination->m_Entity.m_Position;
}

CBotTriangle::CBotTriangle(vec2 a, vec2 b, vec2 c) {
  m_A = a;
  m_B = b;
  m_C = c;

  m_Center = vec2((m_A.x + m_B.x + m_C.x) / 3.0f, (m_A.y + m_B.y + m_C.y) / 3.0f);

  CalculateCircumcenter();
}

double CBotTriangle::D3x3(
	double a11, double a12, double a13,
  double a21, double a22, double a23,
  double a31, double a32, double a33
) {
	return a11*a22*a33 + a12*a23*a31 + a13*a21*a32 - a11*a23*a32 - a12*a21*a33 - a13*a22*a31;
}

void CBotTriangle::CalculateCircumcenter() {
	double D = 2.0 * D3x3(
  	m_A.x, m_A.y, 1,
  	m_B.x, m_B.y, 1,
    m_C.x, m_C.y, 1
  );

  double xc = (1 / D) * D3x3(
  	(m_A.x * m_A.x) + (m_A.y * m_A.y), m_A.y, 1,
  	(m_B.x * m_B.x) + (m_B.y * m_B.y), m_B.y, 1,
  	(m_C.x * m_C.x) + (m_C.y * m_C.y), m_C.y, 1
  );

  double yc = -(1 / D) * D3x3(
  	(m_A.x * m_A.x) + (m_A.y * m_A.y), m_A.x, 1,
  	(m_B.x * m_B.x) + (m_B.y * m_B.y), m_B.x, 1,
  	(m_C.x * m_C.x) + (m_C.y * m_C.y), m_C.x, 1
  );

  m_CircumСenter = vec2(xc, yc);
  m_CircumRadius = distance(m_CircumСenter, m_A);
}

bool CBotTriangle::IsEqual(const CBotTriangle *pOther) const {
  if (m_CircumСenter != pOther->m_CircumСenter || m_CircumRadius != pOther->m_CircumRadius) {
    return false;
  }

  int eq = 0;

  if (m_A == pOther->m_A) eq++;
  if (m_B == pOther->m_A) eq++;
  if (m_C == pOther->m_A) eq++;

  if (m_A == pOther->m_B) eq++;
  if (m_B == pOther->m_B) eq++;
  if (m_C == pOther->m_B) eq++;

  if (m_A == pOther->m_C) eq++;
  if (m_B == pOther->m_C) eq++;
  if (m_C == pOther->m_C) eq++;

  return eq == 3;
}

bool CBotTriangle::IsNeighbor(const CBotTriangle *pOther) const {
  int eq = 0;

  if (m_A == pOther->m_A) eq++;
  if (m_B == pOther->m_A) eq++;
  if (m_C == pOther->m_A) eq++;

  if (m_A == pOther->m_B) eq++;
  if (m_B == pOther->m_B) eq++;
  if (m_C == pOther->m_B) eq++;

  if (m_A == pOther->m_C) eq++;
  if (m_B == pOther->m_C) eq++;
  if (m_C == pOther->m_C) eq++;

  return eq == 2;
}

bool CBotTriangle::IsInside(vec2 point) const {
  float s = m_A.y * m_C.x - m_A.x * m_C.y + (m_C.y - m_A.y) * point.x + (m_A.x - m_C.x) * point.y;
  float t = m_A.x * m_B.y - m_A.y * m_B.x + (m_A.y - m_B.y) * point.x + (m_B.x - m_A.x) * point.y;

  if ((s < 0) != (t < 0))
      return false;

  float A = -m_B.y * m_C.x + m_A.y * (m_C.x - m_B.x) + m_A.x * (m_B.y - m_C.y) + m_B.x * m_C.y;
  if (A < 0.0)
  {
      s = -s;
      t = -t;
      A = -A;
  }
  return s > 0 && t > 0 && (s + t) < A;
}