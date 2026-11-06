#pragma once

class CBlender_overdraw : public IBlender
{
public:
	virtual		LPCSTR		getComment()	{ return "INTERNAL: overdraw debug counting"; }
	virtual		BOOL		canBeDetailed()	{ return FALSE;	}
	virtual		BOOL		canBeLMAPped()	{ return FALSE;	}

	virtual		void		Compile			(CBlender_Compile& C);

	CBlender_overdraw();
	virtual ~CBlender_overdraw();
};
