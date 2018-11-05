#pragma once
#include <Windows.h>
#include <atlstr.h>
#include <unordered_set>
#include <assert.h>
#include <unique_ptr>


/************************************************************************/
/* 为了提高性能使用私有堆来实现buffer                                                                     */
/************************************************************************/
class CReentrantSpinGuard;
template<class CLockObj> class CLocalLock;
template<class T> struct TSimpleList;

//私有堆封装类
class CPrivateHeapImpl
{
public:
	PVOID Alloc(SIZE_T dwSize, DWORD dwFlags = 0)
	{return ::HeapAlloc(m_hHeap, dwFlags, dwSize);}

	PVOID ReAlloc(PVOID pvMemory, SIZE_T dwSize, DWORD dwFlags = 0)
	{return ::HeapReAlloc(m_hHeap, dwFlags, pvMemory, dwSize);}

	SIZE_T Size(PVOID pvMemory, DWORD dwFlags = 0)
	{return ::HeapSize(m_hHeap, dwFlags, pvMemory);}

	BOOL Free(PVOID pvMemory, DWORD dwFlags = 0)
	{return ::HeapFree(m_hHeap, dwFlags, pvMemory);}

	SIZE_T Compact(DWORD dwFlags = 0)
	{return ::HeapCompact(m_hHeap, dwFlags);}

	BOOL IsValid() {return m_hHeap != nullptr;}

	BOOL Reset()
	{
		if(IsValid()) ::HeapDestroy(m_hHeap);
		m_hHeap = ::HeapCreate(m_dwOptions, m_dwInitSize, m_dwMaxSize);

		return IsValid();
	}

public:
	CPrivateHeapImpl(DWORD dwOptions = 0, SIZE_T dwInitSize = 0, SIZE_T dwMaxSize = 0)
		: m_dwOptions(dwOptions), m_dwInitSize(dwInitSize), m_dwMaxSize(dwMaxSize)
	{m_hHeap = ::HeapCreate(m_dwOptions, m_dwInitSize, m_dwMaxSize);}

	~CPrivateHeapImpl	()	{if(IsValid()) ::HeapDestroy(m_hHeap);}

	operator HANDLE	()	{return m_hHeap;}

private:
	CPrivateHeapImpl(const CPrivateHeapImpl&);
	CPrivateHeapImpl operator = (const CPrivateHeapImpl&);

private:
	HANDLE	m_hHeap;
	DWORD	m_dwOptions;
	SIZE_T	m_dwInitSize;
	SIZE_T	m_dwMaxSize;
};

//buffer 基类
template<class T> struct TBufferObjBase
{
	WSAOVERLAPPED		ov;
	WSABUF				buff;

	int					capacity;
	CPrivateHeap&		heap;

	volatile LONG		sndCounter;

	T* next;
	T* last;

	static T* Construct(CPrivateHeap& heap, DWORD dwCapacity)
	{
		T* pBufferObj = (T*)heap.Alloc(sizeof(T) + dwCapacity);
		ASSERT(pBufferObj);

		ZeroMemory(pBufferObj, sizeof(T));

		pBufferObj->TBufferObjBase::TBufferObjBase(heap, dwCapacity);
		pBufferObj->buff.buf = ((char*)pBufferObj) + sizeof(T);

		return pBufferObj;
	}

	static void Destruct(T* pBufferObj)
	{
		ASSERT(pBufferObj);
		pBufferObj->heap.Free(pBufferObj);
	}

	void ResetSendCounter()
	{
		sndCounter = 2;
	}

	LONG ReleaseSendCounter()
	{
		return ::InterlockedDecrement(&sndCounter);
	}

	TBufferObjBase(CPrivateHeap& hp, DWORD dwCapacity)
		: heap(hp), capacity((int)dwCapacity)
	{
		ASSERT(capacity > 0);
	}

	int Cat(const BYTE* pData, int length)
	{
		ASSERT(pData != nullptr && length > 0);

		int cat = min(Remain(), length);

		if(cat > 0)
		{
			memcpy(buff.buf + buff.len, pData, cat);
			buff.len += cat;
		}

		return cat;
	}

	void Reset()	{buff.len = 0;}
	int Remain()	{return capacity - buff.len;}
	BOOL IsFull()	{return Remain() == 0;}
};

//无锁环来控制临界区
template <class T> class CRingPool
{
private:

	typedef T*			TPTR;
	typedef volatile T*	VTPTR;

	static TPTR const E_EMPTY;
	static TPTR const E_LOCKED;
	static TPTR const E_RELEASED;
	static TPTR const E_OCCUPIED;
	static TPTR const E_MAX_STATUS;

private:

	VTPTR& INDEX_VAL(DWORD dwIndex) {return *(m_pv + dwIndex);}

public:

	BOOL TryPut(TPTR pElement)
	{
		ASSERT(pElement != nullptr);

		if(!IsValid()) return FALSE;

		BOOL isOK = FALSE;

		while(true)
		{
			BOOL bOccupy = FALSE;
			DWORD seqPut = m_seqPut;

			if(!HasPutSpace(seqPut))
				break;

			DWORD dwIndex = seqPut % m_dwSize;
			VTPTR& pValue = INDEX_VAL(dwIndex);

			if(pValue == E_RELEASED)
			{
				if(::InterlockedCompareExchangePointer((volatile PVOID*)&pValue, E_OCCUPIED, E_RELEASED) == E_RELEASED)
					bOccupy = TRUE;
				else
					continue;
			}

			if(pValue == E_EMPTY || bOccupy)
			{
				if(::InterlockedCompareExchange(&m_seqPut, seqPut + 1, seqPut) == seqPut)
				{
					pValue	= pElement;
					isOK	= TRUE;

					break;
				}
			}
			else if(pValue == E_LOCKED)
				break;
		}

		return isOK;
	}

	BOOL TryGet(TPTR* ppElement)
	{
		ASSERT(ppElement != nullptr);

		if(!IsValid()) return FALSE;

		BOOL isOK = FALSE;

		while(true)
		{
			DWORD seqGet = m_seqGet;

			if(!HasGetSpace(seqGet))
				break;

			DWORD dwIndex = seqGet % m_dwSize;
			VTPTR& pValue = INDEX_VAL(dwIndex);

			if(pValue == E_LOCKED)
				break;
			else if(pValue != E_EMPTY && pValue != E_RELEASED && pValue != E_OCCUPIED)
			{
				if(::InterlockedCompareExchange(&m_seqGet, seqGet + 1, seqGet) == seqGet)
				{
					ASSERT(pValue > E_MAX_STATUS);

					*(ppElement)	= (TPTR)pValue;
					pValue			= E_EMPTY;
					isOK			= TRUE;

					break;
				}
			}
		}

		return isOK;
	}

	BOOL TryLock(TPTR* ppElement, DWORD& dwIndex)
	{
		ASSERT(ppElement != nullptr);

		if(!IsValid()) return FALSE;

		BOOL isOK = FALSE;

		while(true)
		{
			DWORD seqGet = m_seqGet;

			if(!HasGetSpace(seqGet))
				break;

			dwIndex			= seqGet % m_dwSize;
			VTPTR& pValue	= INDEX_VAL(dwIndex);

			if(pValue == E_LOCKED)
				break;
			else if(pValue != E_EMPTY && pValue != E_RELEASED && pValue != E_OCCUPIED)
			{
				if(::InterlockedCompareExchange(&m_seqGet, seqGet + 1, seqGet) == seqGet)
				{
					ASSERT(pValue > E_MAX_STATUS);

					*(ppElement)	= (TPTR)pValue;
					pValue			= E_LOCKED;
					isOK			= TRUE;

					break;
				}
			}
		}

		return isOK;
	}

	BOOL ReleaseLock(TPTR pElement, DWORD dwIndex)
	{
		ASSERT(dwIndex < m_dwSize);
		ASSERT(pElement == nullptr || pElement > E_MAX_STATUS);

		if(!IsValid()) return FALSE;

		VTPTR& pValue = INDEX_VAL(dwIndex);
		VERIFY(pValue == E_LOCKED);

		if(pElement != nullptr)
		{
			for(DWORD i = 0; ; i++)
			{
				if(TryPut(pElement))
					break;

				DWORD dwPutIndex = m_seqPut % m_dwSize;

				if(dwIndex == dwPutIndex)
				{
					pValue = pElement;
					::InterlockedIncrement(&m_seqPut);

					return TRUE;
				}

				::YieldThread(i);
			}
		}

		pValue = E_RELEASED;

		return TRUE;
	}

public:

	void Reset(DWORD dwSize = 0)
	{
		if(IsValid())
			Destroy();
		if(dwSize > 0)
			Create(dwSize);
	}

	DWORD Size()		{return m_dwSize;}
	DWORD Elements()	{return m_seqPut - m_seqGet;}
	BOOL IsFull()		{return Elements() == Size();}
	BOOL IsEmpty()		{return Elements() == 0;}
	BOOL IsValid()		{return m_pv != nullptr;}

private:

	BOOL HasPutSpace(DWORD seqPut)
	{
		return ((int)(seqPut - m_seqGet) < (int)m_dwSize);
	}

	BOOL HasGetSpace(DWORD seqGet)
	{
		return ((int)(m_seqPut - seqGet) > 0);
	}

	void Create(DWORD dwSize)
	{
		ASSERT(!IsValid() && dwSize > 0);

		m_seqPut = 0;
		m_seqGet = 0;
		m_dwSize = dwSize;
		m_pv	 = (VTPTR*)malloc(m_dwSize * sizeof(TPTR));

		::ZeroMemory(m_pv, m_dwSize * sizeof(TPTR));
	}

	void Destroy()
	{
		ASSERT(IsValid());

		free((void*)m_pv);
		m_pv = nullptr;
		m_dwSize = 0;
		m_seqPut = 0;
		m_seqGet = 0;
	}

public:
	CRingPool(DWORD dwSize = 0)
		: m_pv(nullptr)
		, m_dwSize(0)
		, m_seqPut(0)
		, m_seqGet(0)
	{
		Reset(dwSize);
	}

	~CRingPool()
	{
		Reset(0);
	}

private:
	CRingPool(const CRingPool&);
	CRingPool operator = (const CRingPool&);

private:
	DWORD				m_dwSize;
	VTPTR*				m_pv;
	char				pack1[PACK_SIZE_OF(VTPTR*)];
	volatile DWORD		m_seqPut;
	char				pack2[PACK_SIZE_OF(DWORD)];
	volatile DWORD		m_seqGet;
	char				pack3[PACK_SIZE_OF(DWORD)];
};

template <class T> T* const CRingPool<T>::E_EMPTY		= (T*)0x00;
template <class T> T* const CRingPool<T>::E_LOCKED		= (T*)0x01;
template <class T> T* const CRingPool<T>::E_RELEASED	= (T*)0x02;
template <class T> T* const CRingPool<T>::E_OCCUPIED	= (T*)0x03;
template <class T> T* const CRingPool<T>::E_MAX_STATUS	= (T*)0x0F;

//提供给iocp框架的buffer管理类。使用无锁环来实现高性能
template<class T> const DWORD CNodePoolT<T>::DEFAULT_ITEM_CAPACITY	= ::SysGetPageSize();
template<class T> const DWORD CNodePoolT<T>::DEFAULT_POOL_SIZE		= 300;
template<class T> const DWORD CNodePoolT<T>::DEFAULT_POOL_HOLD		= 1200;

template<class T> class CNodePoolT
{
public:
	void PutFreeItem(T* pItem)
	{
		ASSERT(pItem != nullptr);

		if(!m_lsFreeItem.TryPut(pItem))
			T::Destruct(pItem);
	}

	void PutFreeItem(TSimpleList<T>& lsItem)
	{
		if(lsItem.IsEmpty())
			return;

		T* pItem;
		while((pItem = lsItem.PopFront()) != nullptr)
			PutFreeItem(pItem);
	}

	T* PickFreeItem()
	{
		T* pItem = nullptr;

		if(m_lsFreeItem.TryGet(&pItem))
			pItem->Reset();
		else
			pItem = T::Construct(m_heap, m_dwItemCapacity);
		return pItem;
	}

	inline void Prepare()
	{
		m_lsFreeItem.Reset(m_dwPoolHold);
	}

	inline void Clear()
	{
		T* pItem = nullptr;

		while(m_lsFreeItem.TryGet(&pItem))
			T::Destruct(pItem);

		VERIFY(m_lsFreeItem.IsEmpty());
		m_lsFreeItem.Reset();

		m_heap.Reset();
	}

public:
	void SetItemCapacity(DWORD dwItemCapacity)	{m_dwItemCapacity	= dwItemCapacity;}
	void SetPoolSize	(DWORD dwPoolSize)		{m_dwPoolSize		= dwPoolSize;}
	void SetPoolHold	(DWORD dwPoolHold)		{m_dwPoolHold		= dwPoolHold;}
	DWORD GetItemCapacity	()					{return m_dwItemCapacity;}
	DWORD GetPoolSize		()					{return m_dwPoolSize;}
	DWORD GetPoolHold		()					{return m_dwPoolHold;}

public:
	CNodePoolT(	DWORD dwPoolSize	 = DEFAULT_POOL_SIZE,
		DWORD dwPoolHold	 = DEFAULT_POOL_HOLD,
		DWORD dwItemCapacity = DEFAULT_ITEM_CAPACITY)
		: m_dwPoolSize(dwPoolSize)
		, m_dwPoolHold(dwPoolHold)
		, m_dwItemCapacity(dwItemCapacity)
	{
	}

	~CNodePoolT()	{Clear();}

	DECLARE_NO_COPY_CLASS(CNodePoolT)

public:
	static const DWORD DEFAULT_ITEM_CAPACITY;
	static const DWORD DEFAULT_POOL_SIZE;
	static const DWORD DEFAULT_POOL_HOLD;

private:
	CPrivateHeapImpl	m_heap;

	DWORD			m_dwItemCapacity;
	DWORD			m_dwPoolSize;
	DWORD			m_dwPoolHold;

	CRingPool<T>	m_lsFreeItem;
};

//接收和发送buffer基类模板
template<class T> struct TSimpleList
{
public:
	T* PushFront(T* pItem)
	{
		if(pFront != nullptr)
		{
			pFront->last = pItem;
			pItem->next	 = pFront;
		}
		else
		{
			pItem->last = nullptr;
			pItem->next = nullptr;
			pBack		= pItem;
		}

		pFront = pItem;
		++size;

		return pItem;
	}

	T* PushBack(T* pItem)
	{
		if(pBack != nullptr)
		{
			pBack->next	= pItem;
			pItem->last	= pBack;
		}
		else
		{
			pItem->last = nullptr;
			pItem->next = nullptr;
			pFront		= pItem;
		}

		pBack = pItem;
		++size;

		return pItem;
	}

	T* PopFront()
	{
		T* pItem = pFront;

		if(pFront != pBack)
		{
			pFront = pFront->next;
			pFront->last = nullptr;
		}
		else if(pFront != nullptr)
		{
			pFront	= nullptr;
			pBack	= nullptr;
		}

		if(pItem != nullptr)
		{
			pItem->next = nullptr;
			pItem->last = nullptr;

			--size;
		}

		return pItem;
	}

	T* PopBack()
	{
		T* pItem = pBack;

		if(pFront != pBack)
		{
			pBack = pBack->last;
			pBack->next	= nullptr;
		}
		else if(pBack != nullptr)
		{
			pFront	= nullptr;
			pBack	= nullptr;
		}

		if(pItem != nullptr)
		{
			pItem->next = nullptr;
			pItem->last = nullptr;

			--size;
		}

		return pItem;
	}

	TSimpleList<T>& Shift(TSimpleList<T>& other)
	{
		if(&other != this && other.size > 0)
		{
			if(size > 0)
			{
				pBack->next = other.pFront;
				other.pFront->last = pBack;
			}
			else
			{
				pFront = other.pFront;
			}

			pBack	 = other.pBack;
			size	+= other.size;

			other.Reset();
		}

		return *this;
	}

	void Clear()
	{
		if(size > 0)
		{
			T* pItem;
			while((pItem = PopFront()) != nullptr)
				T::Destruct(pItem);
		}
	}

	T*		Front	()	const	{return pFront;}
	T*		Back	()	const	{return pBack;}
	int		Size	()	const	{return size;}
	bool	IsEmpty	()	const	{return size == 0;}

public:
	TSimpleList()	{Reset();}
	~TSimpleList()	{Clear();}

	DECLARE_NO_COPY_CLASS(TSimpleList<T>)

private:
	void Reset()
	{
		pFront	= nullptr;
		pBack	= nullptr;
		size	= 0;
	}

private:
	int	size;
	T*	pFront;
	T*	pBack;
};

/* Socket 缓冲区基础结构 */
struct TSocketObjBase
{
	static const long DEF_SNDBUFF_SIZE = 8192;

	
	BOOL		connected;
	PVOID		extra;
	PVOID		reserved;
	PVOID		reserved2;
	BOOL		valid;

	union
	{
		DWORD	freeTime;
		DWORD	connTime;
	};

	DWORD		activeTime;

	CCriSec		csSend;

	long			sndBuffSize;
	volatile BOOL	smooth;
	volatile long	pending;
	volatile long	sndCount;

	volatile BOOL	paused;
	volatile BOOL	recving;

	CReentrantSpinGuard csRecv;

	static BOOL IsExist(TSocketObjBase* pSocketObj)
	{return pSocketObj != nullptr;}

	static BOOL IsValid(TSocketObjBase* pSocketObj)
	{return pSocketObj != nullptr && pSocketObj->valid;}

	static void Invalid(TSocketObjBase* pSocketObj)
	{ASSERT(IsExist(pSocketObj)); pSocketObj->valid = FALSE;}

	static void Release(TSocketObjBase* pSocketObj)
	{ASSERT(IsExist(pSocketObj)); pSocketObj->freeTime = ::TimeGetTime();}

	long Pending()		{return pending;}
	BOOL IsPending()	{return pending > 0;}
	BOOL IsCanSend()	{return sndCount <= sndBuffSize;}
	BOOL IsSmooth()		{return smooth;}
	void TurnOnSmooth()	{smooth = TRUE;}

	BOOL TurnOffSmooth()
	{return ::InterlockedCompareExchange((volatile long*)&smooth, FALSE, TRUE) == TRUE;}

	BOOL ResetSndBuffSize(SOCKET socket)
	{
		int len = (int)(sizeof(sndBuffSize));
		return getsockopt(socket, SOL_SOCKET, SO_SNDBUF, (CHAR*)&sndBuffSize, &len) != 0;
	}

	BOOL HasConnected()
	{
		return connected;
	}

	void SetConnected(BOOL bConnected = TRUE)
	{
		connected = bConnected;
	}

	void Reset()
	{
		connected	= FALSE;
		valid		= TRUE;
		smooth		= TRUE;
		paused		= FALSE;
		recving		= FALSE;
		pending		= 0;
		sndCount	= 0;
		sndBuffSize	= DEF_SNDBUFF_SIZE;
		extra		= nullptr;
		reserved	= nullptr;
		reserved2	= nullptr;
	}
};

//用于 SocketObj 缓存
template <class T, class index_type = DWORD, bool adjust_index = false> class CRingCache2
{
public:

	enum EnGetResult {GR_FAIL = -1, GR_INVALID = 0, GR_VALID = 1};

	typedef T*									TPTR;
	typedef volatile T*							VTPTR;

	typedef unordered_set<index_type>			IndexSet;
	typedef typename IndexSet::const_iterator	IndexSetCI;
	typedef typename IndexSet::iterator			IndexSetI;

	static TPTR const E_EMPTY;
	static TPTR const E_LOCKED;
	static TPTR const E_MAX_STATUS;
	static DWORD const MAX_SIZE;

public:

	static index_type& INDEX_INC(index_type& dwIndex)	{if(adjust_index) ++dwIndex; return dwIndex;}
	static index_type& INDEX_DEC(index_type& dwIndex)	{if(adjust_index) --dwIndex; return dwIndex;}

	index_type& INDEX_R2V(index_type& dwIndex)			{dwIndex += *(m_px + dwIndex) * m_dwSize; return dwIndex;}

	BOOL INDEX_V2R(index_type& dwIndex)
	{
		index_type m = dwIndex % m_dwSize;
		BYTE x		 = *(m_px + m);

		if(dwIndex / m_dwSize != x)
			return FALSE;

		dwIndex = m;
		return TRUE;
	}


private:

	VTPTR& INDEX_VAL(index_type dwIndex) {return *(m_pv + dwIndex);}

public:

	BOOL Put(TPTR pElement, index_type& dwIndex)
	{
		ASSERT(pElement != nullptr);

		if(!IsValid()) return FALSE;

		BOOL isOK = FALSE;

		while(true)
		{
			if(!HasSpace())
				break;

			DWORD dwCurSeq			= m_dwCurSeq;
			index_type dwCurIndex	= dwCurSeq % m_dwSize;
			VTPTR& pValue			= INDEX_VAL(dwCurIndex);

			if(pValue == E_EMPTY)
			{
				if(::InterlockedCompareExchangePointer((volatile PVOID*)&pValue, pElement, E_EMPTY) == E_EMPTY)
				{
					::InterlockedIncrement(&m_dwCount);
					::InterlockedCompareExchange(&m_dwCurSeq, dwCurSeq + 1, dwCurSeq);

					dwIndex = INDEX_INC(INDEX_R2V(dwCurIndex));
					isOK	= TRUE;

					if(pElement != E_LOCKED)
						EmplaceIndex(dwIndex);

					break;
				}
			}

			::InterlockedCompareExchange(&m_dwCurSeq, dwCurSeq + 1, dwCurSeq);
		}

		return isOK;
	}

	EnGetResult Get(index_type dwIndex, TPTR* ppElement, index_type* pdwRealIndex = nullptr)
	{
		ASSERT(ppElement != nullptr);

		if(!IsValid() || !INDEX_V2R(INDEX_DEC(dwIndex)))
		{
			*ppElement = nullptr;
			return GR_FAIL;
		}

		*ppElement = (TPTR)INDEX_VAL(dwIndex);
		if(pdwRealIndex) *pdwRealIndex = dwIndex;

		return IsValidElement(*ppElement) ? GR_VALID : GR_INVALID;
	}

	BOOL Set(index_type dwIndex, TPTR pElement, TPTR* ppOldElement = nullptr, index_type* pdwRealIndex = nullptr)
	{
		TPTR pElement2 = nullptr;

		if(pdwRealIndex == nullptr)
			pdwRealIndex = (index_type*)_alloca(sizeof(index_type));

		if(Get(dwIndex, &pElement2, pdwRealIndex) == GR_FAIL)
			return FALSE;

		if(ppOldElement != nullptr)
			*ppOldElement = pElement2;

		if(pElement == pElement2)
			return FALSE;

		int f1 = 0;
		int f2 = 0;

		if(pElement == E_EMPTY)
		{
			if(pElement2 == E_LOCKED)
				f1 = -1;
			else
				f1 = f2 = -1;
		}
		else if(pElement == E_LOCKED)
		{
			if(pElement2 == E_EMPTY)
				f1 = 1;
			else
				f2 = -1;
		}
		else
		{
			if(pElement2 == E_EMPTY)
				f1 = f2 = 1;
			else if(pElement2 == E_LOCKED)
				f2 = 1;
		}

		BOOL bSetValueFirst		= (f1 + f2 >= 0);
		index_type dwRealIndex	= *pdwRealIndex;

		if(bSetValueFirst)	INDEX_VAL(dwRealIndex) = pElement;
		if(f1 > 0)			::InterlockedIncrement(&m_dwCount);
		if(f2 != 0)			(f2 > 0) ? EmplaceIndex(dwIndex) : EraseIndex(dwIndex);
		if(f1 < 0)			{::InterlockedDecrement(&m_dwCount); ++(*(m_px + dwRealIndex));}
		if(!bSetValueFirst) INDEX_VAL(dwRealIndex) = pElement;

		ASSERT(Spaces() <= Size());
		return TRUE;
	}

	BOOL Remove(index_type dwIndex, TPTR* ppElement = nullptr)
	{
		return Set(dwIndex, E_EMPTY, ppElement);
	}

	BOOL AcquireLock(index_type& dwIndex)
	{
		return Put(E_LOCKED, dwIndex);
	}

	BOOL ReleaseLock(index_type dwIndex, TPTR pElement)
	{
		ASSERT(pElement == nullptr || IsValidElement(pElement));

		TPTR pElement2 = nullptr;
		Get(dwIndex, &pElement2);

		ASSERT(pElement2 == E_LOCKED);

		if(pElement2 != E_LOCKED)
			return FALSE;

		return Set(dwIndex, pElement);
	}

public:

	void Reset(DWORD dwSize = 0)
	{
		if(IsValid())
			Destroy();
		if(dwSize > 0)
			Create(dwSize);
	}

	BOOL GetAllElementIndexes(index_type ids[], DWORD& dwCount, BOOL bCopy = TRUE)
	{
		if(ids == nullptr || dwCount == 0)
		{
			dwCount = Elements();
			return FALSE;
		}

		IndexSet* pIndexes = nullptr;
		IndexSet indexes;

		if(bCopy)
			pIndexes = &CopyIndexes(indexes);
		else
			pIndexes = &m_indexes;

		BOOL isOK	 = FALSE;
		DWORD dwSize = (DWORD)pIndexes->size();

		if(dwSize > 0 && dwSize <= dwCount)
		{
			IndexSetCI it  = pIndexes->begin();
			IndexSetCI end = pIndexes->end();

			for(int i = 0; it != end; ++it, ++i)
				ids[i] = *it;

			isOK = TRUE;
		}

		dwCount = dwSize;

		return isOK;
	}

	unique_ptr<index_type[]> GetAllElementIndexes(DWORD& dwCount, BOOL bCopy = TRUE)
	{
		IndexSet* pIndexes = nullptr;
		IndexSet indexes;

		if(bCopy)
			pIndexes = &CopyIndexes(indexes);
		else
			pIndexes = &m_indexes;

		unique_ptr<index_type[]> ids;
		dwCount = (DWORD)pIndexes->size();

		if(dwCount > 0)
		{
			ids.reset(new index_type[dwCount]);

			IndexSetCI it  = pIndexes->begin();
			IndexSetCI end = pIndexes->end();

			for(int i = 0; it != end; ++it, ++i)
				ids[i] = *it;
		}

		return ids;
	}

	static BOOL IsValidElement(TPTR pElement) {return pElement > E_MAX_STATUS;}

	DWORD Size		()	{return m_dwSize;}
	DWORD Elements	()	{return (DWORD)m_indexes.size();}
	DWORD Spaces	()	{return m_dwSize - m_dwCount;}
	BOOL HasSpace	()	{return m_dwCount < m_dwSize;}
	BOOL IsEmpty	()	{return m_dwCount == 0;}
	BOOL IsValid	()	{return m_pv != nullptr;}

private:

	void Create(DWORD dwSize)
	{
		ASSERT(!IsValid() && dwSize > 0 && dwSize <= MAX_SIZE);

		m_dwCurSeq	= 0;
		m_dwCount	= 0;
		m_dwSize	= dwSize;
		m_pv		= (VTPTR*)malloc(m_dwSize * sizeof(TPTR));
		m_px		= (BYTE*)malloc(m_dwSize * sizeof(BYTE));

		::ZeroMemory(m_pv, m_dwSize * sizeof(TPTR));
		::ZeroMemory(m_px, m_dwSize * sizeof(BYTE));
	}

	void Destroy()
	{
		ASSERT(IsValid());

		m_indexes.clear();
		free((void*)m_pv);
		free((void*)m_px);

		m_pv		= nullptr;
		m_px		= nullptr;
		m_dwSize	= 0;
		m_dwCount	= 0;
		m_dwCurSeq	= 0;
	}

	IndexSet& CopyIndexes(IndexSet& indexes)
	{
		{
			CReadLock locallock(m_cs);
			indexes = m_indexes;
		}

		return indexes;
	}

	void EmplaceIndex(index_type dwIndex)
	{
		CWriteLock locallock(m_cs);
		m_indexes.emplace(dwIndex);
	}

	void EraseIndex(index_type dwIndex)
	{
		CWriteLock locallock(m_cs);
		m_indexes.erase(dwIndex);
	}

public:
	CRingCache2	(DWORD dwSize = 0)
		: m_pv		(nullptr)
		, m_px		(nullptr)
		, m_dwSize	(0)
		, m_dwCount	(0)
		, m_dwCurSeq(0)
	{
		Reset(dwSize);
	}

	~CRingCache2()
	{
		Reset(0);
	}

private:
	CRingCache2(const CRingCache2&);
	CRingCache2 operator = (const CRingCache2&);

private:
	DWORD				m_dwSize;
	VTPTR*				m_pv;
	char				pack1[PACK_SIZE_OF(VTPTR*)];
	BYTE*				m_px;
	char				pack2[PACK_SIZE_OF(BYTE*)];
	volatile DWORD		m_dwCurSeq;
	char				pack3[PACK_SIZE_OF(DWORD)];
	volatile DWORD		m_dwCount;
	char				pack4[PACK_SIZE_OF(DWORD)];

	CSimpleRWLock		m_cs;
	IndexSet			m_indexes;
};

template <class T, class index_type, bool adjust_index> T* const CRingCache2<T, index_type, adjust_index>::E_EMPTY		= (T*)0x00;
template <class T, class index_type, bool adjust_index> T* const CRingCache2<T, index_type, adjust_index>::E_LOCKED		= (T*)0x01;
template <class T, class index_type, bool adjust_index> T* const CRingCache2<T, index_type, adjust_index>::E_MAX_STATUS	= (T*)0x0F;

//重入自旋锁实现，考虑到onvif业务会多线程调用，和iocp里多线程对事件的处理
//那么在一个线程里要保证网络数据的连贯性，不至于被多线程弄乱。
#pragma intrinsic(_ReadBarrier)
#pragma intrinsic(_WriteBarrier)
#pragma intrinsic(_ReadWriteBarrier)

#define DEFAULT_CRISEC_SPIN_COUNT	4096

#if defined (_WIN64)
#define DEFAULT_PAUSE_RETRY		16
#define DEFAULT_PAUSE_YIELD		128
#define DEFAULT_PAUSE_CYCLE		8192
#else
#define DEFAULT_PAUSE_RETRY		4
#define DEFAULT_PAUSE_YIELD		32
#define DEFAULT_PAUSE_CYCLE		4096
#endif

#ifndef YieldProcessor
#pragma intrinsic(_mm_pause)
#define YieldProcessor _mm_pause
#endif

static inline void YieldThread(UINT i = DEFAULT_PAUSE_RETRY)
{
	if		(i < DEFAULT_PAUSE_RETRY)		;
	else if	(i < DEFAULT_PAUSE_YIELD)		YieldProcessor();
	else if	(i < DEFAULT_PAUSE_CYCLE - 1)	SwitchToThread();
	else if	(i < DEFAULT_PAUSE_CYCLE)		Sleep(0);
	else									YieldThread(i & (DEFAULT_PAUSE_CYCLE - 1));
}

static inline void YieldThread(UINT i = DEFAULT_PAUSE_RETRY)
{
	if		(i < DEFAULT_PAUSE_RETRY)		;
	else if	(i < DEFAULT_PAUSE_YIELD)		YieldProcessor();
	else if	(i < DEFAULT_PAUSE_CYCLE - 1)	SwitchToThread();
	else if	(i < DEFAULT_PAUSE_CYCLE)		Sleep(0);
	else									YieldThread(i & (DEFAULT_PAUSE_CYCLE - 1));
}

class CReentrantSpinGuard
{
public:
	CReentrantSpinGuard()
		: m_dwThreadID	(0)
		, m_iCount		(0)
	{

	}

	~CReentrantSpinGuard()
	{
		ASSERT(m_dwThreadID	== 0);
		ASSERT(m_iCount		== 0);
	}

	void Lock()
	{
		for(UINT i = 0; !_TryLock(i == 0); ++i)
			YieldThread(i);
	}

	BOOL TryLock()
	{
		return _TryLock(TRUE);
	}

	void Unlock()
	{
		ASSERT(m_dwThreadID == ::GetCurrentThreadId());

		if((--m_iCount) == 0)
			m_dwThreadID = 0;
	}

private:
	CReentrantSpinGuard(const CReentrantSpinGuard& cs);
	CReentrantSpinGuard operator = (const CReentrantSpinGuard& cs);

	BOOL _TryLock(BOOL bFirst)
	{
		DWORD dwCurrentThreadID = ::GetCurrentThreadId();

		if(bFirst && m_dwThreadID == dwCurrentThreadID)
		{
			++m_iCount;
			return TRUE;
		}

		if(::InterlockedCompareExchange(&m_dwThreadID, dwCurrentThreadID, 0) == 0)
		{
			::_ReadWriteBarrier();
			ASSERT(m_iCount == 0);

			m_iCount = 1;

			return TRUE;
		}

		return FALSE;
	}

private:
	volatile DWORD	m_dwThreadID;
	int				m_iCount;
};

//锁使用help
template<class CLockObj> class CLocalLock
{
public:
	CLocalLock(CLockObj& obj) : m_lock(obj) {m_lock.Lock();}
	~CLocalLock() {m_lock.Unlock();}
private:
	CLockObj& m_lock;
};