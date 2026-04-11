import '../styles/Contents.css';
import '../styles/Permission.css';

export const PermissionsView = ({ roles, loading, connected }) => {
  if (loading) {
    return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>Loading data from Backend ...</h1>;
  }

  const serverStatus = connected
    ? "🟢"
    : "🔴";

  return (
    <div className="permissions-container">
      <div className="permissions-header">
        <h1>
          System decentralization |
        </h1>
        <h2>
          Server status: {serverStatus}
        </h2>
      </div>
      <div className="permissions-role-list">
        {roles.map((role, index) => (
          <div key={index} className="permissions-role-item">
            <span className="permissions-role-dot"></span>
            <span className="permissions-role-name">{role}</span>
          </div>
        ))}
      </div>
    </div>
  );
};

export default PermissionsView;