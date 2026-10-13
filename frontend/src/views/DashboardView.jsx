import '../styles/Contents.css';
import '../styles/Dashboard.css';

export const DashboardView = ({ stats, loading, connected }) => {
  if (loading) {
    return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>Loading data from Backend ...</h1>;
  }

  const serverStatus = connected
    ? "🟢"
    : "🔴";

  return (
    <div className="dashboard-container">
      <div className="dashboard-header">
        <h2>
          System overview
        </h2>
        <h6>
          {serverStatus}
        </h6>
      </div>
      <div className="dashboard-stats-grid">
        <div className="dashboard-stat-card">
          <h3>Total departments</h3>
          <p className="dashboard-stat-number dashboard-color-dept">{stats.dept}</p>
        </div>
        <div className="dashboard-stat-card">
          <h3>Total staffs</h3>
          <p className="dashboard-stat-number dashboard-color-total">{stats.total}</p>
        </div>
        <div className="dashboard-stat-card">
          <h3>On boarding</h3>
          <p className="dashboard-stat-number dashboard-color-active">{stats.active}</p>
        </div>
        <div className="dashboard-stat-card">
          <h3>Off boarding</h3>
          <p className="dashboard-stat-number dashboard-color-inactive">{stats.inactive}</p>
        </div>
      </div>
    </div >
  );
};

export default DashboardView;