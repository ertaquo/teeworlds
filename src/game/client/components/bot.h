#ifndef GAME_CLIENT_COMPONENTS_BOT_H
#define GAME_CLIENT_COMPONENTS_BOT_H

#include <base/vmath.h>
#include <game/client/component.h>

#include <list>
#include <map>
#include <utility>
#include <atomic>
#include <mutex>

/*

*/

struct CBotEntity {
	public:
		ivec2 m_FieldPosition;
		vec2 m_Position;
		int m_Type;

    CBotEntity() {
    }

    CBotEntity(vec2 position, int type) {
      m_Position = position;
      m_Type = type;
      m_FieldPosition = ivec2((int)round(position.x / 32.0f), (int)round(position.y / 32.0f));
    }

    CBotEntity(ivec2 fieldPosition, int type) {
      m_FieldPosition = fieldPosition;
      m_Type = type;
      m_Position = vec2(fieldPosition.x * 32.0f + 16.0f, fieldPosition.y * 32.0f + 16.0f);
    }
};

enum {
  ENTITY_BOT_EXTENDED = NUM_ENTITIES,
  ENTITY_FLAG_CARRIED_RED,
  ENTITY_FLAG_CARRIED_BLUE,
  ENTITY_ENEMY
};

struct CBotTriangle {
  public:
    vec2 m_A, m_B, m_C;
    vec2 m_Center;
    vec2 m_CircumСenter;
    double m_CircumRadius;

    std::list<const CBotTriangle*> m_aNeighbors;
    //std::list<const CBotTriangle*> m_aVisible;

    CBotTriangle(vec2 a, vec2 b, vec2 c);

    bool IsEqual(const CBotTriangle *pOther) const;
    bool IsNeighbor(const CBotTriangle *pOther) const;

    bool IsInside(vec2 point) const;
  private:
    double D3x3(
      double a11, double a12, double a13,
      double a21, double a22, double a23,
      double a31, double a32, double a33
    );
    void CalculateCircumcenter();
};

class CBot : public CComponent {
private:
  int m_FieldWidth, m_FieldHeight;
  char *m_pField;
	std::list<CBotEntity> m_aEntities;
  std::list<vec2> m_aEdges;

  std::atomic<int> m_TriangulationID;
  std::mutex m_TrianglesLock;
  std::list<CBotTriangle*> m_aTriangles;
  std::atomic<bool> m_bTrianglesReady;
  std::map<std::pair<const CBotTriangle *, const CBotTriangle *>, std::list<vec2>> m_PathCache;

  int m_aWeapons[NUM_WEAPONS];
  int m_WeaponPickup;

  CNetObj_PlayerInput m_PreviousInput;

  vec2 m_Destination;

  void ResetWeapons();

  void LoadMapData();

  void FindEdges();
  void RenderEdges();

  void ResetTriangles(std::list<CBotTriangle*> &triangles);
  void TriangulateMap(int ID, std::list<vec2> edges);
  void FindTrianglesNeighbors(bool useLock = true);
  void RenderTriangles();
  const CBotTriangle * FindTriangle(vec2 point, bool useLock = true);
  const CBotTriangle * FindNearestTriangle(vec2 point, bool useLock = true);
  const CBotTriangle * FindNearestVisibleTriangle(vec2 point, bool useLock = true);

  void SaveTriangles(bool useLock = true);
  bool LoadTriangles(bool useLock = true);

  std::list<vec2> FindPath(vec2 from, vec2 to, bool useDirect = true);
  void RenderPath(std::list<vec2> path);
  float GetPathDistance(std::list<vec2> path);
  float GetPathDistance(vec2 from, vec2 to);
  int GetPathWaypointsCount(std::list<vec2> path);
  int GetPathWaypointsCount(vec2 from, vec2 to);
  bool ValidatePath(std::list<vec2> path);

  CNetObj_PlayerInput Move(const int iterationsCount = 2, const int predictionTicks = 8);

  bool IsTeamplay();
  bool IsCTF();
  bool IsEnemy(int clientID);

  std::list<CNetObj_Character> GetEnemies();
  const std::tuple<float, float, float> GetWeaponTuning(int weapon); // curvature, speed, lifetime
  CNetObj_PlayerInput Shoot(CNetObj_PlayerInput currentInput);

  std::list<CBotEntity> GetActiveEntities();
  void ChooseDestination();

  inline int GetFieldIndex(int x, int y) { return x + y * m_FieldWidth; };
	inline ivec2 GetPositionFromIndex(int index) { return ivec2(index % m_FieldWidth, index / m_FieldHeight); }
public:
	CBot();

	virtual void OnReset();
	virtual void OnRelease();
	virtual void OnRender();
	virtual void OnMessage(int MsgType, void *pRawMsg);
	virtual void OnPlayerDeath();
	virtual void OnStartGame();

	CNetObj_PlayerInput GetInputData(CNetObj_PlayerInput lastData);
};

#endif
