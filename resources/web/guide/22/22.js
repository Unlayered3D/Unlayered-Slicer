function OnInit()
{
	TranslatePage();
	
	RequestProfile();
}

function HandleStudio(pVal)
{
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		m_ProfileItem=pVal['response'];
		SortUI();
	}
	else if(strCmd=='page_before_filaments')
	{
		if( pVal['page']=='21update' )
		{
			if( m_LeftFilaments )
				return;
			m_LeftFilaments=true;
			document.location.href="../21update/index.html";
		}
		else
			ReturnPreviewPageDefault();
	}
}

// Unlayered 0.1.8: Back goes to "Update your Unlayered printer" (guide 21update) when the wizard
// shows it; the slicer knows (page_before_filaments, HandleStudio above).
function ReturnPreviewPage()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_page_before_filaments";
	SendWXMessage( JSON.stringify(tSend) );
	// Should no answer come, the old way.
	setTimeout(ReturnPreviewPageDefault, 15000);
}

var m_LeftFilaments=false;

function ReturnPreviewPageDefault()
{
	if( m_LeftFilaments )
		return;
	m_LeftFilaments=true;

	/* ORCA the profile reply is held until the filament data is loaded, so this can fire before
	   m_ProfileItem exists. Fall back to the printer picker rather than throwing - page 22 is the
	   last page of the wizard and the first-run dialog has no close box. */
	if(!m_ProfileItem)
	{
		document.location.href="../21/index.html";
		return;
	}

	let nMode=m_ProfileItem["model"].length;
	
	if( nMode==1)
		document.location.href="../1/index.html";
	else
		document.location.href="../21/index.html";	
}

function FinishGuide()
{
	/* ORCA the filament selection still has to be saved here -- ResponseFilamentResult() posts it and
	   refuses to advance when nothing is ticked. The wizard now ends on page 25 (the multiplanar
	   tutorial offer), which is what sends user_guide_finish. */
	let bRet=ResponseFilamentResult();

	if(bRet)
		window.location.href="../25/index.html";
}
